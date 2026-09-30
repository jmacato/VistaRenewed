# Vista host control service

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


`TritonVistaControl` is an automatic LocalSystem service for the local Vista test
VM. It uses COM2, backed by a host Unix socket. It does not require networking,
Remote Desktop, the Triton graphics driver, or an interactive logon for system
commands. The old `TritonVistaDeploy` helper remains disabled.

The socket directory must belong to the host VM owner and have mode `0700`.
This is the authorization boundary: anyone able to connect can issue SYSTEM
commands. The current VM uses `vista-kvm/x64-base/control/control.sock` with mode
`0600`, inside a directory with mode `0700`. Do not expose this channel over TCP.

## Use

Run from the repository root:

```sh
python3 tools/vista_control.py ping
python3 tools/vista_control.py run 'whoami & sc query TritonVistaControl'
python3 tools/vista_control.py run --user --timeout 600 'E:\fullscreen.cmd'
python3 tools/vista_control.py get 'C:\Windows\Temp\triton9-ddi.log' /tmp/ddi.log
python3 tools/vista_control.py put /tmp/probe.cmd 'C:\probe.cmd'
```

`run` uses SYSTEM. `run --user` uses the active console user's token and desktop;
for an administrator it uses the linked elevated token when available. It fails
if that session is unavailable rather than silently running in session 0.
System commands remain available in Safe Mode. Interactive launches depend on
Windows session services and are intended for normal-mode graphics testing.

The client prints a persistent job ID before submission. One job runs at a time.
Use `--detach`, then `status ID`, for long operations. Reusing `run --id ID`
queries the saved job instead of executing a second time. A disconnect never
causes the client to resubmit a command. Job commands, output and status remain
in `C:\ProgramData\TritonControl\jobs`; `get` can retrieve the output after a
reconnect or reboot. A job interrupted by service restart is reported as
interrupted, not automatically resumed. A job object kills descendants when the
job ends or its timeout expires.

Uploads are limited to 1 MiB per request; use the VM's mounted ISO for larger
packages. Downloads stream in 256 KiB chunks. Commands are UTF-8 on the wire and
converted to the guest OEM encoding for cmd.exe; unrepresentable commands fail.
Binary file contents are transferred unchanged. Local drive paths are required.

## Build and install

### UI Automation desktop worker

The host client exposes UIA primitives through the existing USER job operation.
The worker runs on the active interactive desktop with the existing elevated
user-token policy, not in SYSTEM's session-0 desktop. Each operation has a
20-second job limit, so a hung accessibility provider does not block serial
service processing indefinitely. The existing one-active-job restriction still
applies: this version cannot inspect an app while another control job is active.

```sh
podman exec -w /workspace vista-driver-builder bash scripts/build_vista_uia.sh
python3 tools/vista_transfer.py put build/vista-uia.exe 'C:\ProgramData\TritonControl\vista-uia.exe'
python3 tools/vista_control.py uia probe
python3 tools/vista_control.py uia tree --hwnd 0
python3 tools/vista_control.py uia tree --hwnd 1234
python3 tools/vista_control.py uia focus --hwnd 1234 --path 0.2 --expect-name 'Address'
python3 tools/vista_control.py uia invoke --hwnd 1234 --path 0.3 --expect-name 'Refresh'
python3 tools/vista_control.py uia set-value --hwnd 1234 --path 0.2 --expect-name 'Address' --value 'about:blank'
```

HWNDs are hexadecimal. Tree output contains JSON records with child paths,
names, automation IDs, control types, PID, focus/enabled state and bounds.
Traversal is capped at depth 8 and 256 nodes. Paths are transient: re-read the
tree after UI changes. Mutations reject a changed element name, but duplicate
names are possible; root HWND and path are not durable identity. Text arguments
are hex-encoded before crossing cmd.exe. Invoke uses InvokePattern, set-value
uses ValuePattern; neither silently falls back to coordinate clicks.

On the current Vista target, job `21358ff3699c48689e4f2a311a475c5d` reports
`uia_available:false`, HRESULT `0x80040154` (class not registered). Therefore
tree/action execution is not yet verified here. The COM UIA client requires
Vista SP2 plus the Platform Update; this change does not install that update.
Reference: https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomation

Host validation: `python3 tests/test_vista_uia_client.py`.

### TCP bulk uploads

Use `vista_transfer.py` for development binaries and large packages. The command
still travels over serial, but the file streams over HTTP/TCP through QEMU's
`10.0.2.2` gateway. No reboot or guest firewall rule is required. The temporary
server binds only to loopback inside the QEMU container, serves exactly one
random-token URL, and exits after the transfer. The SYSTEM control protocol is
**not** exposed over TCP. This is a trusted local VM channel, not an encrypted
network distribution service.

One-time bootstrap:

```sh
podman exec -w /workspace vista-driver-builder i686-w64-mingw32-gcc -std=c11 -Os -Wall -Wextra -Werror -municode -s tools/vista_fetch.c -o build/vista-fetch.exe -lwinhttp -ladvapi32
python3 tools/vista_control.py put build/vista-fetch.exe 'C:\ProgramData\TritonControl\vista-fetch.exe'
```

Subsequent uploads:

```sh
python3 tools/vista_transfer.py put build/triton-ie7-mshtml-activation-probe.dll 'C:\TritonSupermiumBridge\triton-ie7-mshtml-activation-probe.dll'
```

The helper streams to a unique adjacent temporary file, verifies the SHA-256
sent through the private control channel, flushes, then replaces the destination.
A bad hash leaves the destination unchanged. Unlike serial `put`, there is no
1 MiB payload limit; the command has a 600-second transfer deadline. The source
must be available at the same absolute path inside the VM container's workspace
mount. `--container` selects a different QEMU container. Downloads back to the
host still use serial `get` for now.

Verified on the Vista development VM: a 490,872-byte DLL transferred in 1.11s,
and an intentionally wrong hash preserved the original destination bytes.

### Control service

```sh
podman exec -w /workspace vista-driver-builder bash scripts/build_vista_control_linux.sh
```

This builds `build/triton-vista-control.exe` with the Vista API target, legacy
msvcrt, and a PE/import compatibility audit. Run the executable with `--install`
from an elevated guest command prompt once. It copies itself into System32 and
starts the service automatically. No kernel signing policy changes are needed.

Connect the second emulated serial port to the private socket:

```text
-serial file:/path/to/com1.log
-serial unix:/private/directory/control.sock,server=on,wait=off
```

The current VM's saved QEMU command has this configuration. Its socket parent
remains private across restarts even if QEMU recreates the socket file.

The installer registers the service in SafeBoot Minimal and Network, together
with Serial/serial.sys, Serenum/serenum.sys and the Ports device class. Registering
only the service was insufficient in Vista RTM: the service started but the
serial driver remained stopped. Minimal Safe Mode has been exercised with a
SYSTEM command reporting `SafeBoot\Option\OptionValue=1` and both services running.

For updates, upload the new executable to a temporary guest path and execute it
with `--stage-update`, then reboot. It schedules replacement through Windows
Session Manager and clears the destination's read-only attribute inherited from
installation media. Verify the new version/binary after reboot; scheduling is
not proof that the replacement happened.

`--uninstall` stops/deletes the service and removes its own SafeBoot entries.
It leaves the executable, saved jobs, and serial/Ports SafeBoot entries in place,
so it does not delete test evidence or remove transport settings another recovery
workflow may rely on.

## Protocol and verification

Requests are `TC1 ID OP ARG LENGTH SHA256\n` followed by LENGTH raw bytes.
IDs are 32 lowercase hexadecimal characters. SHA256 covers the payload; an
incomplete/corrupted payload cannot become a valid command. Responses are
`TC1 ID STATUS CODE LENGTH\n` followed by raw bytes. Status is OK, RUNNING,
DONE or ERROR. DONE carries the process exit code; ERROR carries a Win32 code.
Supported operations are PING, RUN, USER, STATUS, READ and WRITE.

```sh
python3 tests/test_vista_control.py
```

These tests cover fragmented binary responses, stale response IDs, truncated
and oversized responses, guest errors, and no command replay after disconnect.
The VM tests additionally exercise SYSTEM identity, interactive session launch,
file transfer, persisted job status across reboots, and Safe Mode transport.

The client holds an exclusive advisory lock beside the socket for its entire
connection. A second client fails before connecting: the QEMU serial chardev
cannot safely multiplex independent streams. WRITE requests extend the socket
timeout to at least 120 seconds for the possible 1 MiB transfer at 115200 baud,
then restore the caller's timeout. Prefer mounting an ISO for large binaries.
