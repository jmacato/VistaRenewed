# Vista control and file transfer

The control service runs commands and transfers files over the VM's second
serial port. IE installation and Sidebar updates use this service.

## Build and install

```sh
bash scripts/dev-container.sh run bash scripts/build_vista_control_linux.sh
```

Copy `build/triton-vista-control.exe` into Vista using mounted media. Run it
with `--install` from an elevated guest command prompt.

`run-vm.sh` supplies the serial socket. For another QEMU setup, add:

```text
-serial file:/path/to/com1.log
-serial unix:/private/directory/control.sock,server=on,wait=off
```

Keep the socket directory private to the VM owner (`0700`). Anyone who can
connect can run SYSTEM commands. Do not expose the socket over TCP.

## Use

Run from the repo root:

```sh
python3 tools/vista_control.py ping
python3 tools/vista_control.py run 'whoami'
python3 tools/vista_control.py run --user --timeout 30 'whoami'
python3 tools/vista_control.py put /tmp/example.txt 'C:\Windows\Temp\example.txt'
python3 tools/vista_control.py get 'C:\Windows\Temp\example.txt' /tmp/copy.txt
```

`run` uses SYSTEM. `run --user` uses the active user's desktop and the linked
admin token when available. For another VM, place
`--socket /absolute/path/to/control.sock` before the command.

Only one job runs at a time. Use `run --detach` for a long job, then
`status JOB_ID` to check it. The service keeps job output and status under
`C:\ProgramData\TritonControl\jobs`. `run --id JOB_ID` returns the saved
status of that job. A job's child processes stop when the job ends or times out.

## Larger uploads

Serial uploads are limited to 1 MiB. For larger files, use mounted media or
build the HTTP fetch helper:

```sh
bash scripts/dev-container.sh run i686-w64-mingw32-gcc -std=c11 -Os -Wall -Wextra -Werror -municode -s tools/vista_fetch.c -o build/vista-fetch.exe -lwinhttp -ladvapi32
python3 tools/vista_control.py put build/vista-fetch.exe 'C:\ProgramData\TritonControl\vista-fetch.exe'
python3 tools/vista_transfer.py put build/example.dll 'C:\Windows\Temp\example.dll'
```

The source file and transfer script must be mounted at the same absolute
paths inside the VM container. `--container NAME` selects another container;
this helper uses the default control socket. Transfers check SHA-256 before
replacing the destination. The temporary HTTP server listens only inside the
VM container; the control service stays on serial.

## Service updates

Stage a new executable with `--stage-update`, then reboot to replace the
installed service. `--uninstall` removes the service registration and its own
Safe Mode entries. Saved jobs and serial transport settings remain.
