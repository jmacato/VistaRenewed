# Restoring Vista Sidebar data providers

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


Vista's original RSS, Weather, and Currency pages remain in place. This
integration replaces only the retired data providers with a local relay and
compatibility adapters, so the existing Vista artwork, layout, controls, and
flyouts are retained.

## Sources

- News: Google News RSS for the Philippines in English.
- Weather and location search: Open-Meteo forecast and geocoding APIs.
- Currency: Frankfurter rates, with USD as the base rate and the complete
  source currency set exposed through the original Currency chooser.

None of these sources requires a key. The relay caches news and weather for
ten minutes, location searches for one day, and currency rates for twelve
hours. If an upstream source is temporarily unreachable, it serves its most
recent cached response.

## Launch and install

`run-vm.sh` starts the loopback-only relay inside the QEMU container before it
launches the guest. QEMU user networking exposes that listener to Vista at
`10.0.2.2:8765`; the guest therefore uses HTTP only on its host-only network
path, while the relay uses current HTTPS support for upstream sources.

After the Vista control service is available, install the adapters and restart
Sidebar with:

```sh
python3 tools/update_vista_sidebar_gadgets.py
```

The installer first backs up the exact original gadget pages under
`C:\ProgramData\TritonSidebarGadgets\original`, refuses to proceed unless
the active pages match those backups, and changes only the RSS, Weather, and
Currency data scripts. It starts Sidebar in the active `triton` console
session and configures the task to start it at logon.

Weather starts in Manila when no previous location is saved. Its original
settings page can search for and save a different city. The RSS flyout accepts
only sanitized paragraphs, lists, emphasis, and HTTP(S) links from the feed;
active markup and arbitrary attributes are discarded. Currency codes with no
Vista-era localized name use their three-letter ISO code rather than showing
`null`.

## Verification

Run both checks after installation:

```sh
python3 tools/verify_vista_sidebar_gadgets.py --source
python3 tools/verify_vista_sidebar_gadgets.py --guest
```

The guest check compares all three active HTML pages with their original
backups, validates the installed data adapters, tests news, forecast, location
search, currency rates, and fallback currency labels through Vista's own
XMLHTTP stack, and confirms that Sidebar is running.
