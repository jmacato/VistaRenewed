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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import vista_sidebar_paths as paths
import update_vista_sidebar_gadgets as updater


class SidebarTests(unittest.TestCase):
    def test_location_validation(self):
        for code in ('bad', '91;0;Invalid', '0;181;Invalid', 'nan;0;Invalid'):
            with self.assertRaises(ValueError):
                proxy.location_from_code(code)
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

    def test_language_folders_keep_separate_backups(self):
        output = '\n'.join('SIDEBAR_FOLDER|' + gadget + '|D:\\Program Files\\Windows Sidebar\\Gadgets\\' + gadget + '\\' + locale
                           for gadget in paths.GADGETS for locale in ('en-US', 'de-DE', 'ja-JP'))
        pages, backends = paths.gadget_files(paths.parse_folders(output))
        self.assertEqual(len(pages), 9)
        self.assertEqual(len(backends), 12)
        self.assertEqual(len(set(pages.values())), 9)
        self.assertEqual(len({name for name, kind in backends.values()}), 12)
        self.assertIn('RSSFeeds.html', pages.values())
        self.assertIn('rss.backend-original.js', [name for name, kind in backends.values()])
        self.assertIn('Weather.Gadget\\de-DE\\weather.html', pages.values())

    def test_missing_or_unsafe_language_folders_fail(self):
        for output in ('', 'SIDEBAR_FOLDER|Weather.Gadget|C:\\Weather.Gadget\\..',
                       'SIDEBAR_FOLDER|Weather.Gadget|C:\\Weather.Gadget\\de-DE&bad'):
            with self.assertRaises(RuntimeError):
                paths.parse_folders(output)

    def test_neutral_gadget_root(self):
        output = '\n'.join('SIDEBAR_FOLDER|' + name + '|C:\\Gadgets\\' + name for name in paths.GADGETS)
        pages, backends = paths.gadget_files(paths.parse_folders(output))
        self.assertEqual(len(pages), 3)
        self.assertEqual(len(backends), 4)

    def test_weather_patch_does_not_depend_on_english_comments(self):
        source = '''var city = getLocalizedString('DefaultCity');
try {
 // Wetterdienst verbinden
 var oMSN = new ActiveXObject("wlsrvc.WLServices");
 this.oMSN = oMSN.GetService("weather");
} catch (error) { this.isValid = false; }
this.oMSN.OnDataReady = onDataReadyHandler;'''
        patched = updater.patched_weather_script(source.encode('utf-16')).decode('utf-16')
        self.assertIn("getLocalizedString('DefaultCity')", patched)
        self.assertNotIn('wlsrvc.WLServices', patched)
        self.assertIn('this.oMSN.OnDataReady = onDataReadyHandler;', patched)
        settings = '''var theWeatherLocation = unescape(readSetting("WeatherLocation")) || gDefaultWeatherLocation;
var oMSN = new ActiveXObject("wlsrvc.WLServices");
MicrosoftGadget.oMSN = oMSN.GetService("weather");'''
        patched = updater.patched_weather_settings_script(settings.encode('utf-16')).decode('utf-16')
        self.assertIn(settings.splitlines()[0], patched)
        self.assertNotIn('Manila', patched)
        self.assertNotIn('wlsrvc.WLServices', patched)

    def test_news_uses_guest_language_and_region(self):
        payload = b'<rss><channel><title>Nachrichten</title><item><description>News</description></item></channel></rss>'
        with patch.object(proxy, 'fetch_bytes', return_value=payload) as fetch:
            proxy.fetch_news({'locale': ['de-DE']})
            parameters = proxy.parse_qs(proxy.urlsplit(fetch.call_args.args[0]).query)
            self.assertEqual(parameters, {'hl': ['de-DE'], 'gl': ['DE'], 'ceid': ['DE:de']})
        self.assertEqual(proxy.locale_parts({'locale': ['zh-Hant-TW']}), ('zh', 'TW'))
        with self.assertRaises(ValueError):
            proxy.locale_parts({'locale': ['de-DE&bad']})

    def test_weather_search_uses_guest_language(self):
        with patch.object(proxy, 'fetch_bytes', return_value=b'{"results": []}') as fetch:
            proxy.fetch_weather_search({'query': ['Berlin'], 'locale': ['de-DE']})
            parameters = proxy.parse_qs(proxy.urlsplit(fetch.call_args.args[0]).query)
            self.assertEqual(parameters['language'], ['de'])

    def test_legacy_weather_code_uses_saved_city(self):
        locations = b'<locations><location code="52.52;13.405;Berlin"/></locations>'
        with patch.object(proxy, 'fetch_weather_search', return_value=locations) as search, \
             patch.object(proxy, 'fetch_bytes', return_value=b'{}') as fetch, \
             patch.object(proxy, 'weather_xml', return_value=b'weather') as render:
            self.assertEqual(proxy.fetch_weather({'location': ['wc:GMXX0007'], 'name': ['Berlin'], 'locale': ['de-DE']}), b'weather')
            search.assert_called_once_with({'query': ['Berlin'], 'locale': ['de-DE']})
            parameters = proxy.parse_qs(proxy.urlsplit(fetch.call_args.args[0]).query)
            self.assertEqual(parameters['latitude'], ['52.52'])
            render.assert_called_once_with({}, 'Berlin')
