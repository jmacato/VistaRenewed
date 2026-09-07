# Offline Vista evidence inspection

Inspection time: `2026-08-26T05:18:28+0800` through
`2026-08-26T05:26:42+0800`.

Scope: Read the inactive disk at
`/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2` in place.
The inspection did not start or control QEMU.

## Result

The latest deployment ID is
`b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4`.
The registry and deployment log contain this same ID.

The public D3D9 probe did not pass. The probe completed a target-only clear.
The next call, the D24S8 depth clear, failed with `0x80004005` (`E_FAIL`).
The probe did not reach clear readback, triangle readback, or `PresentEx`.

This report makes no rendering, DWM, Aero, or glass claim.

## Inactive-disk proof

The first disk read started in the same second as the following checks.

- Exact-process recheck: `2026-08-26T05:18:28+0800`. No configured process was present.
- Any-QEMU disk recheck: no QEMU process had the exact disk path.
- Open-holder recheck: `lsof` returned no row for the exact disk.
- Safe to inspect: yes

The configured-process identity required all three fields:

- executable: `/Users/jumar/winvistachecked/triton-qemu/build/qemu-system-x86_64`.
- VM name: `vista-aero-guest-deploy`.
- disk path: `/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2`.

The check used these commands:

```sh
vista_disk=/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2
configured_qemu=/Users/jumar/winvistachecked/triton-qemu/build/qemu-system-x86_64
process_table=$(ps -ww -axo pid=,ppid=,lstart=,stat=,command=)
printf '%s\n' "$process_table" | awk -v exe="$configured_qemu" -v disk="$vista_disk" \
  '$9 == exe && index($0, "-name vista-aero-guest-deploy") && index($0, disk) {print}'
printf '%s\n' "$process_table" | awk -v disk="$vista_disk" \
  '{exe=$9; n=split(exe,p,"/"); base=p[n]; if ((index(base,"qemu-system-")==1 || base=="qemu-kvm" || base=="qemu-storage-daemon") && index($0,disk)) print}'
lsof -nP -- "$vista_disk"
```

The exact output was:

```text
RECHECK_TIME=2026-08-26T05:18:28+0800
EXACT_CONFIGURED_PROCESS=NONE
ANY_QEMU_DISK_PROCESS=NONE
OPEN_HOLDER=NONE
SAFE_TO_INSPECT=yes
READ_START=2026-08-26T05:18:28+0800
```

The extraction recheck at `2026-08-26T05:19:26+0800` had the same empty
result. The payload-list recheck at `2026-08-26T05:21:50+0800` also had the
same result.

## Read-only method

Free space before first disk access: `df -h .` showed `78 GiB` available.
The exact `df -k .` value was `81,543,072 KiB` available.

The installed p7zip `7z` reader is version `17.05`.
It traversed QCOW2, MBR, and NTFS without a mount.
It reported a `42,949,672,960`-byte virtual disk and one NTFS partition.
The partition starts at byte `1,048,576`.

Read-only command for the first in-place listing:

```sh
7z l -slt /Users/jumar/winvistachecked/winvista-3.shrunk.qcow2
```

Read-only command for the bounded extraction:

```sh
extract_dir=$(mktemp -d /tmp/triton-offline-evidence.XXXXXX)
7z x -bd -y -o"$extract_dir" \
  /Users/jumar/winvistachecked/winvista-3.shrunk.qcow2 \
  'Windows/Temp/triton-deploy.log' \
  'Windows/Temp/triton9-service.log' \
  'Windows/Temp/triton9-service-stage.log' \
  'Windows/Temp/triton9-probe.log' \
  'Windows/Temp/triton9-ddi.log' \
  'Windows/Temp/triton9-d3d9-proof.log' \
  'triton9-ddi.log' \
  'Windows/System32/config/SOFTWARE'
```

The command extracted eight named files and `31,531,210` bytes.
The temporary directory used `30,812 KiB` of host space.
No DWM or Aero file was in the extraction command.

The installed `python-registry` reader opened the extracted hive read-only.
The reader used this operation:

```python
from Registry import Registry
hive = Registry.Registry(software_hive_path)
key = hive.open(r'Triton\VistaDeploy')
for value in key.values():
    print(value.name(), value.value_type_str(), value.value(), value.raw_data())
```

The following SHA-256 values identify the temporary evidence before removal:

| Named file | Bytes | SHA-256 |
|---|---:|---|
| `triton-deploy.log` | 72,708 | `f7e388293b847959650169d7e1a7438faea9a395b3942775ef8bd1d9bec97d21` |
| `triton9-service.log` | 71,981 | `bd41ea43ec3558153e054526efc63a0c2691e42d6aa85be2202170657130ea9b` |
| `triton9-probe.log` | 4,158 | `cd9dfe4e225214f3da6d6e3f864b7def7e8cd4188dd91ac9a4bd3de4873fb8b9` |
| `triton9-d3d9-proof.log` | 504 | `706f4591147220627ec6ebb69e4612fbc2801211ab57b86d71e27f92af96180a` |
| temporary `triton9-ddi.log` | 12,843 | `4fb25edea9f855e62eeed70f23c2f4a0f171a14812994dbb36eaab750ed0151c` |
| root `triton9-ddi.log` | 1,484,093 | `44022491cec79f4f159499b2e66930934c2fee4090f20a68edbb9b092bf93f8d` |
| `SOFTWARE` | 29,884,416 | `114a3c7498eb9a4f1ce5142c17178ba694d4cdd09f283580b0f6cc4ea915a349` |

The stage log was also extracted. It was `507` bytes.

Mount write: none  
Overlay created: none  
Raw conversion: none  
Disk copy: none

## Timestamp rules

`TRITONDEPLOY` lines contain guest-written UTC timestamps with a `Z` suffix.
Those timestamps are the exact guest event times in this report.

Registry key times are Windows FILETIME values. The hive reader decoded the
target key time as `2026-08-16T12:57:07.702330Z`.

The `7z` reader displayed NTFS times in the host timezone, which is `+0800`.
These values came from the guest filesystem. They are not host file-event
times for this inspection.

The temporary extraction preserved the displayed NTFS modification times.
The extraction time is not a guest event time.

## Latest deployment

Deployment ID:
`b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4`.

The complete latest deployment sequence in `triton-deploy.log` is:

```text
TRITONDEPLOY 2026-08-16T12:50:44Z INSTALL_BEGIN id=b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4 mode=reboot-payload
TRITONDEPLOY 2026-08-16T12:50:44Z INSTALL_STAGE id=b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4 step=schedule-payload
TRITONDEPLOY 2026-08-16T12:50:45Z INSTALL_PAYLOAD_OK file=triton-vista-deploy.exe
TRITONDEPLOY 2026-08-16T12:52:36Z INSTALL_OK id=b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4 reboot-required=1
TRITONDEPLOY 2026-08-16T12:52:36Z DEPLOY_COMPLETE id=b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4
TRITONDEPLOY 2026-08-16T12:56:55Z NORMAL_READY
TRITONDEPLOY 2026-08-16T12:57:01Z PROBE_LAUNCH_FAIL id=b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4 error=1072
TRITONDEPLOY 2026-08-16T12:57:07Z PROBE_LAUNCHED id=b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4
TRITONDEPLOY 2026-08-16T18:00:55Z NORMAL_READY
```

Deployment state: the log records a normal-mode, delayed-payload schedule.
It records `reboot-required=1`, then a later normal-ready state.
The first probe launch failed with Win32 error `1072`.
The next probe launch succeeded six seconds later.

Probe launch: `PROBE_LAUNCHED` occurred at `2026-08-16T12:57:07Z`.
The scoped service name is `TritonD3D9Probe_b4bf74b765f6`.

### SOFTWARE hive state

The `Triton\VistaDeploy` key contained exactly six values:

| Value | Type | Exact data |
|---|---|---|
| `AttemptId` | `REG_SZ` | `b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4` |
| `LastSuccessId` | `REG_SZ` | `b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4` |
| `Result` | `REG_SZ` | `REBOOTING_NORMAL_OK` |
| `InstallOutcome` | `REG_SZ` | `OK` |
| `SafeModeOwned` | `REG_SZ` | Unicode control value `U+0001` |
| `ProbeLaunchId` | `REG_SZ` | `b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4` |

The raw `SafeModeOwned` data was `01 00 00 00`.
This value is not the text `0` or `1`.
Thus, the field is present but not a valid documented text state.
This report makes no Safe Mode state inference from this field.

The key had no other value. Thus, any unlisted pending-state field was absent.
The key last-write time was `2026-08-16T12:57:07.702330Z`.
The full hive NTFS time was `2026-08-17T08:37:38+0800`.
That full-hive time does not change the earlier target-key time.

### Install verification and payload identity

Install verification: the latest log proves scheduling, not final-byte
verification. It contains no `VERIFY_*` marker and no post-reboot byte check.
The full 650-line deployment log contains no line with `VERIFY`.

The pre-install verifier does not write a separate success field to this log.
Thus, `INSTALL_BEGIN` is not an independent hash-verification result.

Only `triton-vista-deploy.exe` has an `INSTALL_PAYLOAD_OK` line for the latest
ID. The other three payload names have no success or failure field in that
latest sequence. Their state is absent from that sequence, not failed.

Payload identity on the inactive disk is present as this NTFS metadata:

| Guest path | Bytes | NTFS modified time shown in `+0800` |
|---|---:|---|
| `C:\Windows\System32\drivers\viogpu3d.sys` | 205,296 | `2026-08-16T03:51:59+0800` |
| `C:\Windows\System32\neptune_d3d9.dll` | 20,955,094 | `2026-08-16T13:29:53+0800` |
| `C:\Windows\SysWOW64\neptune_d3d9_wow.dll` | 19,123,281 | `2026-08-16T13:29:53+0800` |
| `C:\Windows\System32\triton-vista-deploy.exe` | 307,526 | `2026-08-16T13:49:05+0800` |
| `C:\Windows\Temp\TritonD3D9Probe_b4bf74b765f6.exe` | 593,556 | `2026-08-16T13:49:05+0800` |

These tuples identify the files that are present. They do not prove a
cryptographic match to the missing deployment media.

## Public D3D9 probe

The service log copied the full public transcript from offset `0` with size
`4,158`. This size equals the extracted probe file size.
Thus, the absence results below are from a complete transcript.

The exact terminal sequence is:

```text
SetViewport                    hr=0x00000000 PASS
Clear offscreen target         ENTER
Clear offscreen target         hr=0x00000000 PASS
Clear D24S8 depth              ENTER
Clear D24S8 depth              hr=0x80004005 FAIL
TRITON9-PROBE FAIL
```

- Last completed boundary: `Clear D24S8 depth`, failed with `0x80004005`.
- Last successful boundary: `Clear offscreen target`, with `0x00000000`.
- HRESULT/status: `0x80004005` (`E_FAIL`). The secure process exit code was `1`.
- Clear pixel: absent. The probe did not reach `Clear readback`.
- Triangle inside: absent. The protocol calls this field `triangle center`.
- Triangle outside: absent.
- PresentEx result: absent.
- Terminal marker: `TRITON9-PROBE FAIL` is present once.
- Pass marker: `TRITON9-PROBE PASS` is absent.
- Guest timestamp: absent inside `triton9-probe.log`.

The nearest exact guest timestamp is the launch line at
`2026-08-16T12:57:07Z`. The probe file NTFS time was displayed as
`2026-08-16T20:57:08+0800`, which equals `2026-08-16T12:57:08Z`.
This NTFS time is a file time, not an internal probe timestamp.

D3D9 gate status: FAIL. The clear/readback, triangle/readback, and PresentEx
gate did not pass.

## DDI and proof-event correlation

DDI correlation: the service started the secure child as PID `2212`.
The hexadecimal form is `0x000008a4`.
The service-captured DDI tail contains five proof records for PID
`000008a4` and build `20260816`.

The bounded DDI header records offset `1,409,365` and size `1,474,901`.
The later extracted root log was `1,484,093` bytes.
Thus, `9,192` bytes were appended after the service captured its tail.

The last scoped DDI device sequence records these results:

- `TRITON9-OPENADAPTER success private-info=deferred`.
- `TRITON9-CREATEDEVICE success`.
- A `128x128` format `0x15` render target.
- A `128x128` format `0x4b` depth-stencil resource.
- A `128x128` format `0x15` system-memory readback resource.
- `TRITON9-PRIVATE-DEFERRED-CHECK-HR=00000000`.
- `TRITON9-HOST-PROXY success`.

The captured DDI tail contains no explicit DDI failure marker for the depth
clear. Thus, it does not identify the internal cause of `0x80004005`.

Proof-event correlation: the service captured a `280`-byte proof tail.
It contains five build-and-PID pairs for build `20260816` and PID
`000008a4`. These pairs match the secure child exactly.

The current root DDI and proof files contain later records for other PIDs.
The root DDI NTFS time was `2026-08-17T02:01:10+0800`.
The proof-file NTFS time was `2026-08-17T02:01:09+0800`.
These times are approximately five hours after the probe file time.
This report does not attribute those later records to the public probe.

## DWM evidence decision

DWM evidence decision: excluded because the public D3D9 gate failed.
The bounded extraction omitted the Aero probe and WinSAT DWM logs.
The service decision was `AERO probe skipped D3D9 gate failed`.

VISUAL-UNVERIFIED remains in force. No DWM API result, Aero result, visual
result, rendering result, or glass result is in this report.

## Safety and cleanup

Temporary extraction removed: yes  
VM control: none  
VM mutation: none  
Raw derivative: none  
Raw framebuffer: none  
Source files modified: none  
Boot/UI retrieval: none

The inspection did not start, stop, pause, reset, or drive a VM.
It did not use QMP, HMP, host input, a mount, or GUI automation.
It did not create an overlay, disk copy, raw partition, screenshot, or
framebuffer.

## Four-pass review

1. The first pass inspected the named evidence and recorded all required
   public-probe fields.
2. The expert pass separated scheduled deployment from post-reboot
   verification. It also correlated the probe PID with the DDI proof PID.
3. The defect pass found the malformed `SafeModeOwned` text value. It also
   found that the DDI files contain later, unrelated PIDs.
4. The polish pass separated guest UTC events, NTFS file times, and host
   inspection times. It made no claim from an absent field.
