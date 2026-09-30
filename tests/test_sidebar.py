"""Offline Sidebar provider contracts; no VM or upstream requests."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('sidebar_proxy', Path(__file__).resolve().parents[1] / 'packaging/vista-sidebar-gadgets/vista_sidebar_proxy.py')
proxy = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = proxy
spec.loader.exec_module(proxy)


class SidebarTests(unittest.TestCase):
    def test_location_validation(self):
        for code in ('bad', '91;0;Invalid', '0;181;Invalid', 'nan;0;Invalid'):
            self.assertEqual(proxy.location_from_code(code), proxy.DEFAULT_LOCATION)
        self.assertEqual(proxy.location_from_code('14.6;121.0;Manila'), (14.6, 121.0, 'Manila'))

    def test_weather_contract(self):
        document = dict(latitude=14.6, longitude=121, current=dict(temperature_2m=28.6, weather_code=0, is_day=0), daily=dict(time=['2026-09-30', '2026-10-01', '2026-10-02', '2026-10-03'], temperature_2m_max=[31]*4, temperature_2m_min=[25]*4, weather_code=[0]*4))
        root = ET.fromstring(proxy.weather_xml(document, 'Manila & Bay'))
        self.assertEqual(root.attrib['location'], 'Manila & Bay')
        self.assertEqual(root.attrib['temperature'], '29')
        self.assertEqual(root.attrib['skyCode'], '31')
        self.assertEqual(len(root.findall('forecast')), 4)
        document['daily']['time'] = []
        with self.assertRaises(ValueError):
            proxy.weather_xml(document, 'Manila')

    def test_feed_removes_active_content(self):
        value = proxy.sanitize_feed_description('<script>alert(1)</script><a href="javascript:bad()">bad</a><img src=x onerror="bad()"><b>Headline</b>')
        self.assertNotIn('<script', value)
        self.assertNotIn('javascript:', value)
        self.assertNotIn('onerror', value)
        self.assertIn('Headline', value)

    def test_cache_and_stale_fallback(self):
        fetch = Mock(return_value=b'fresh')
        source = proxy.Source('application/xml', 10, fetch)
        cache = proxy.Cache()
        with patch.dict(proxy.SOURCES, {'/test': source}), patch.object(proxy.time, 'monotonic', return_value=0):
            self.assertEqual(cache.get('/test', {}), (b'fresh', False))
            self.assertEqual(cache.get('/test', {}), (b'fresh', False))
            self.assertEqual(fetch.call_count, 1)
        fetch.side_effect = OSError('offline')
        with patch.dict(proxy.SOURCES, {'/test': source}), patch.object(proxy.time, 'monotonic', return_value=20):
            self.assertEqual(cache.get('/test', {}), (b'fresh', True))
            with self.assertRaises(OSError):
                cache.get('/test', {'location': ['other']})
