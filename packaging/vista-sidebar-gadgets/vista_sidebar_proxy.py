#!/usr/bin/env python3
"""Serve cached, fixed-purpose data providers for Vista Sidebar gadgets."""

from __future__ import annotations

import argparse
import datetime
import html
import json
import re
import threading
import time
import xml.etree.ElementTree as element_tree
from dataclasses import dataclass
from html.parser import HTMLParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable
from urllib.error import URLError
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import Request, urlopen


USER_AGENT = "TritonVistaSidebar/1.0 (+local cached relay)"
Parameters = dict[str, list[str]]


@dataclass(frozen=True)
class Source:
    content_type: str
    cache_seconds: int
    fetch: Callable[[Parameters], bytes]


def one(parameters: Parameters, name: str) -> str:
    values = parameters.get(name, [])
    if len(values) != 1:
        return ""
    return values[0]


def fetch_bytes(url: str) -> bytes:
    request = Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/xml, application/json"})
    with urlopen(request, timeout=20) as response:
        payload = response.read(1024 * 1024 + 1)
    if len(payload) > 1024 * 1024:
        raise ValueError("upstream response exceeds 1 MiB")
    return payload


class FeedDescriptionSanitizer(HTMLParser):
    """Keep simple article structure while rejecting feed-supplied active HTML."""

    _allowed_tags = {"a", "b", "blockquote", "br", "em", "i", "li", "ol", "p", "strong", "ul"}

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []

    def handle_starttag(self, tag: str, attributes: list[tuple[str, str | None]]) -> None:
        tag = tag.lower()
        if tag not in self._allowed_tags:
            return
        if tag == "a":
            href = next((value for name, value in attributes if name.lower() == "href"), "") or ""
            parsed = urlsplit(href)
            if parsed.scheme.lower() in {"http", "https"}:
                self.parts.append('<a href="' + html.escape(href, quote=True) + '" target="_blank">')
                return
            self.parts.append("<a>")
            return
        self.parts.append("<" + tag + ">")

    def handle_startendtag(self, tag: str, attributes: list[tuple[str, str | None]]) -> None:
        self.handle_starttag(tag, attributes)
        if tag.lower() != "br":
            self.handle_endtag(tag)

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if tag in self._allowed_tags and tag != "br":
            self.parts.append("</" + tag + ">")

    def handle_data(self, data: str) -> None:
        self.parts.append(html.escape(data, quote=False))


def sanitize_feed_description(fragment: str) -> str:
    sanitizer = FeedDescriptionSanitizer()
    sanitizer.feed(fragment)
    sanitizer.close()
    return "".join(sanitizer.parts)


def weather_code(code: int, is_day: int) -> tuple[int, str]:
    """Map Open-Meteo WMO weather codes to Vista's original MSN icon codes."""
    if code == 0:
        return (32 if is_day else 31, "Clear")
    if code == 1:
        return (34 if is_day else 33, "Mostly clear")
    if code == 2:
        return 30, "Partly cloudy"
    if code == 3:
        return 26, "Cloudy"
    if code in (45, 48):
        return 20, "Fog"
    if code in (51, 53, 55, 56, 57):
        return 9, "Drizzle"
    if code in (61, 63, 65, 66, 67, 80, 81, 82):
        return 11, "Rain"
    if code in (71, 73, 75, 77, 85, 86):
        return 16, "Snow"
    if code in (95, 96, 99):
        return 4, "Thunderstorms"
    return 44, "Unavailable"


def location_from_code(code: str) -> tuple[float, float, str]:
    if not code:
        raise ValueError("weather needs a valid location")
    parts = code.split(";", 2)
    if len(parts) != 3 or len(parts[2].strip()) == 0 or len(parts[2]) > 100:
        raise ValueError("weather needs a valid location")
    try:
        latitude, longitude = float(parts[0]), float(parts[1])
    except ValueError:
        raise ValueError("weather needs a valid location")
    if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
        raise ValueError("weather needs a valid location")
    return latitude, longitude, parts[2].strip()


def weather_xml(document: dict[str, object], location: str) -> bytes:
    current = document.get("current", {})
    daily = document.get("daily", {})
    if not isinstance(current, dict) or not isinstance(daily, dict):
        raise ValueError("weather response lacks required objects")
    if not isinstance(current.get("temperature_2m"), (int, float)):
        raise ValueError("weather response has no current temperature")
    dates = daily.get("time", [])
    highs = daily.get("temperature_2m_max", [])
    lows = daily.get("temperature_2m_min", [])
    codes = daily.get("weather_code", [])
    if not all(isinstance(values, list) for values in (dates, highs, lows, codes)) or min(len(dates), len(highs), len(lows), len(codes)) < 4:
        raise ValueError("weather response has fewer than four forecast days")
    legacy_code, text = weather_code(int(current["weather_code"]), int(current.get("is_day", 1)))
    result = element_tree.Element(
        "weather",
        {
            "location": location,
            "latitude": str(document["latitude"]),
            "longitude": str(document["longitude"]),
            "temperature": str(round(current["temperature_2m"])),
            "skyCode": str(legacy_code),
            "skyText": text,
            "url": "http://10.0.2.2:8765/weather",
            "attribution": "Weather data: Open-Meteo",
        },
    )
    for index in range(4):
        day_code, day_text = weather_code(int(codes[index]), 1)
        element_tree.SubElement(
            result,
            "forecast",
            {
                "date": str(dates[index]),
                "day": ("Today" if index == 0 else datetime.date.fromisoformat(str(dates[index])).strftime("%a")),
                "high": str(round(highs[index])),
                "low": str(round(lows[index])),
                "skyCode": str(day_code),
                "skyText": day_text,
            },
        )
    return element_tree.tostring(result, encoding="utf-8", xml_declaration=True)


def fetch_weather(parameters: Parameters) -> bytes:
    try:
        latitude, longitude, location = location_from_code(one(parameters, "location"))
    except ValueError:
        # Vista's original MSN location codes cannot be used by Open-Meteo.
        # Resolve the saved/default localized city name instead.
        locations = element_tree.fromstring(fetch_weather_search({
            "query": [one(parameters, "name")], "locale": [one(parameters, "locale")],
        }))
        first = locations.find("location")
        if first is None:
            raise ValueError("weather location was not found")
        latitude, longitude, location = location_from_code(first.attrib["code"])
    query = urlencode(
        {
            "latitude": latitude,
            "longitude": longitude,
            "current": "temperature_2m,weather_code,is_day",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min",
            "timezone": "auto",
            "forecast_days": 4,
        }
    )
    payload = fetch_bytes("https://api.open-meteo.com/v1/forecast?" + query)
    return weather_xml(json.loads(payload.decode("utf-8")), location)


def fetch_weather_search(parameters: Parameters) -> bytes:
    query = one(parameters, "query").strip()
    if not query or len(query) > 100:
        raise ValueError("weather search needs a short location query")
    payload = fetch_bytes(
        "https://geocoding-api.open-meteo.com/v1/search?"
        + urlencode({"name": query, "count": 5, "language": locale_parts(parameters)[0], "format": "json"})
    )
    document = json.loads(payload.decode("utf-8"))
    results = document.get("results", [])
    if not isinstance(results, list):
        raise ValueError("weather search response has no results")
    root = element_tree.Element("locations")
    for item in results:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            continue
        latitude, longitude = item.get("latitude"), item.get("longitude")
        if not isinstance(latitude, (int, float)) or not isinstance(longitude, (int, float)):
            continue
        name = item["name"]
        country = str(item.get("country", ""))
        fullname = name + (", " + country if country else "")
        element_tree.SubElement(
            root,
            "location",
            {
                "name": name,
                "fullname": fullname,
                "code": f"{latitude};{longitude};{name}",
            },
        )
    return element_tree.tostring(root, encoding="utf-8", xml_declaration=True)


def locale_parts(parameters: Parameters) -> tuple[str, str]:
    locale = one(parameters, "locale") or "en-US"
    if not re.fullmatch(r"[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*", locale):
        raise ValueError("invalid language tag")
    parts = locale.split("-")
    region = next((part.upper() for part in parts[1:] if len(part) == 2 and part.isalpha()), "US")
    return parts[0].lower(), region


def fetch_news(parameters: Parameters) -> bytes:
    language, region = locale_parts(parameters)
    query = urlencode({"hl": language + "-" + region, "gl": region, "ceid": region + ":" + language})
    payload = fetch_bytes("https://news.google.com/rss?" + query)
    document = element_tree.fromstring(payload)
    if document.tag.lower().split("}")[-1] != "rss" or not document.findall(".//item"):
        raise ValueError("news response has no RSS items")
    for description in document.findall(".//description"):
        description.text = sanitize_feed_description(description.text or "")
    return element_tree.tostring(document, encoding="utf-8", xml_declaration=True)


def fetch_currency(_: Parameters) -> bytes:
    payload = fetch_bytes("https://api.frankfurter.dev/v2/rates?base=USD")
    document = json.loads(payload.decode("utf-8"))
    entries = [item for item in document if isinstance(item, dict) and isinstance(item.get("quote"), str)]
    quotes = {item["quote"] for item in entries}
    if not {"PHP", "EUR", "JPY", "GBP"}.issubset(quotes):
        raise ValueError("currency response is missing expected currencies")
    result = element_tree.Element("rates", {"base": "USD", "date": str(entries[0]["date"])})
    element_tree.SubElement(result, "rate", {"symbol": "USD", "value": "1"})
    for item in sorted(entries, key=lambda rate: rate["quote"]):
        element_tree.SubElement(result, "rate", {"symbol": item["quote"], "value": str(item["rate"])})
    return element_tree.tostring(result, encoding="utf-8", xml_declaration=True)


SOURCES = {
    "/news": Source("application/xml; charset=utf-8", 600, fetch_news),
    "/weather": Source("application/xml; charset=utf-8", 600, fetch_weather),
    "/weather-search": Source("application/xml; charset=utf-8", 86400, fetch_weather_search),
    "/currency": Source("application/xml; charset=utf-8", 43200, fetch_currency),
}


class Cache:
    def __init__(self) -> None:
        self._entries: dict[str, tuple[float, bytes]] = {}
        self._lock = threading.Lock()

    def get(self, path: str, parameters: Parameters) -> tuple[bytes, bool]:
        source = SOURCES[path]
        key = path + "?" + urlencode(sorted((name, value) for name, values in parameters.items() for value in values))
        with self._lock:
            entry = self._entries.get(key)
        if entry and time.monotonic() - entry[0] < source.cache_seconds:
            return entry[1], False
        try:
            payload = source.fetch(parameters)
        except (OSError, URLError, ValueError, UnicodeDecodeError, json.JSONDecodeError, element_tree.ParseError):
            if entry:
                return entry[1], True
            raise
        with self._lock:
            self._entries[key] = (time.monotonic(), payload)
        return payload, False

    def status(self) -> dict[str, bool]:
        with self._lock:
            return {path.lstrip("/"): any(key.startswith(path + "?") for key in self._entries) for path in SOURCES}


CACHE = Cache()


class Handler(BaseHTTPRequestHandler):
    server_version = "VistaSidebarRelay/1.0"

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        url = urlsplit(self.path)
        if url.path == "/health":
            self._send(200, "application/json; charset=utf-8", json.dumps({"ok": True, "cached": CACHE.status()}).encode("utf-8"))
            return
        if url.path not in SOURCES:
            self._send(404, "application/json; charset=utf-8", b'{"error":"not found"}')
            return
        try:
            payload, stale = CACHE.get(url.path, parse_qs(url.query, keep_blank_values=True))
        except Exception:
            self._send(502, "application/json; charset=utf-8", b'{"error":"upstream unavailable"}')
            return
        headers = {"Cache-Control": "max-age=60", "X-Data-Stale": "1" if stale else "0"}
        self._send(200, SOURCES[url.path].content_type, payload, headers)

    def _send(self, code: int, content_type: str, payload: bytes, headers: dict[str, str] | None = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: object) -> None:
        return


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8765, type=int)
    args = parser.parse_args()
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
