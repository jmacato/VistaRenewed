# Windows Sidebar setup

The providers replace the retired RSS, weather and currency services. They
keep Vista's original gadget pages and back up files before changes.

## Start the relay

`run-vm.sh` starts the relay inside the QEMU container. Vista reaches it at
`http://10.0.2.2:8765`. Set `VISTA_SIDEBAR=0` to disable it.

The relay uses Google News RSS, Open-Meteo weather and location search, and
Frankfurter currency rates. It connects to them over HTTPS; no API key is
needed. RSS defaults to Philippine news in English. Weather defaults to
Manila when no saved location is available.

## Update the guest

First set up the [Vista control service](VISTA_CONTROL.md), then run:

```sh
python3 tools/update_vista_sidebar_gadgets.py --user 'DOMAIN\username'
```

Use the Windows account that owns Sidebar. The updater checks the original
HTML against its backups, replaces the data scripts and restarts Sidebar.
Use `--no-restart` to leave it running.

Backups are stored in `C:\ProgramData\TritonSidebarGadgets\original`.
Use the original weather settings page to select another city.

## Check the result

```sh
python3 tools/verify_vista_sidebar_gadgets.py --source
python3 tools/verify_vista_sidebar_gadgets.py --guest
```

The source check runs without a VM. The guest check tests the providers,
checks the original pages and confirms that Sidebar is running.

For another VM, pass `--socket /absolute/path/to/control.sock` to the updater
and verifier, plus `--vm-name NAME` to the guest verifier.

The relay caches news and weather for ten minutes, location searches for
one day, and rates for twelve hours. It uses the last cached response when
an upstream service is unavailable. RSS descriptions are sanitized before
being sent to the gadget.
