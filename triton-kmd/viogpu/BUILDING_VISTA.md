# Build Vista Aero Packages

This procedure builds the Vista x86 and x64 `viogpu3d` packages. It requires a
Windows build host with a Vista-compatible WDK and matching `virtiolib.lib`.

The macOS host builds the D3D9 UMDs. The Windows host builds and signs the KMD
packages. Do not use a modern WDK library path for the Vista KMD.

## Configure the Windows build host

1. Open a Developer Command Prompt that has the Vista-compatible WDK toolset.
2. Set `VISTA_WDK_INC_PATH` to the Vista WDK `inc\ddk` directory.
3. Set `VISTA_WDK_API_PATH` to the Vista WDK `inc\api` directory.
4. Set `VISTA_WDK_CRT_PATH` to the Vista WDK `inc\crt` directory.
5. Set `VISTA_WDK_LIB_PATH` to the matching Vista WDK library directory.
6. Set `VISTA_VIRTIOLIB_PATH` to the matching Vista build of `virtiolib.lib`.
7. Copy the audited D3D9 UMD files to paths that the Windows host can read.

For the x86 package, set `VISTA_D3D9_UMD` to the x86 UMD. For the x64 package,
set `VISTA_D3D9_UMD` to the x64 UMD and `VISTA_D3D9_WOW_UMD` to the x86 UMD.

## Build and create catalogs

Run these commands from the `triton-kmd\\viogpu` directory.

Run the x86 build with these commands:

```bat
set "VISTA_D3D9_UMD=D:\triton\neptune_d3d9_x86.dll"
msbuild viogpu_vista.sln /m /p:Configuration="Vista x86" /p:Platform=Win32
```

Run the x64 build with these commands:

```bat
set "VISTA_D3D9_UMD=D:\triton\neptune_d3d9_x64.dll"
set "VISTA_D3D9_WOW_UMD=D:\triton\neptune_d3d9_x86.dll"
msbuild viogpu_vista.sln /m /p:Configuration="Vista x64" /p:Platform=x64
```

The build creates these package directories:

```text
Install\Vista\x86
Install\Vista\amd64
```

PackOne runs `Inf2Cat` with `Vista_X86` or `Vista_X64`. It creates the catalog
name declared by the selected Vista INF.

## Run package audits

Run these commands from the `triton-kmd` directory after each build:

```bat
py -3 viogpu\tools\check_vista_kmd_source.py

py -3 viogpu\tools\check_vista_pe.py --kind kmd --arch x86 viogpu\objfre_vista_x86\i386\viogpu3d.sys
py -3 viogpu\tools\check_vista_inf.py --arch x86 viogpu\Install\Vista\x86\viogpu3d.inf --package-dir viogpu\Install\Vista\x86

py -3 viogpu\tools\check_vista_pe.py --kind kmd --arch x64 viogpu\objfre_vista_amd64\amd64\viogpu3d.sys
py -3 viogpu\tools\check_vista_inf.py --arch x64 viogpu\Install\Vista\amd64\viogpu3d.inf --package-dir viogpu\Install\Vista\amd64
```

Run the UMD audit for each file before you stage it in a package.

```bat
py -3 viogpu\tools\check_vista_pe.py --kind umd --arch x86 neptune_d3d9_x86.dll
py -3 viogpu\tools\check_vista_pe.py --kind umd --arch x64 neptune_d3d9_x64.dll
```

## Test-sign the catalogs

Keep the test certificate and private key outside the repository. Install the
test certificate in the test VM trusted-root store. Then sign each catalog
with the test certificate from the Windows certificate store.

```bat
signtool sign /fd SHA1 /sha1 <test-certificate-thumbprint> Install\Vista\x86\viogpu3d-vista-x86.cat
signtool sign /fd SHA1 /sha1 <test-certificate-thumbprint> Install\Vista\amd64\viogpu3d-vista-x64.cat
```

On Vista x64, enable test signing before you install the package:

```bat
bcdedit /set testsigning on
```

Restart the VM after this command.

## Run the direct QEMU test

The Triton QEMU binary is `triton-qemu\build\qemu-system-x86_64`. It supports
the required `neptune`, `blob`, and `hostmem` properties.

The command below opens `winvista-3.shrunk.qcow2` directly. It writes to that
image. Use `-vga std` during installation or recovery. Use `-vga none` after
the Vista driver is installed.

```sh
/Users/jumar/winvistachecked/triton-qemu/build/qemu-system-x86_64 \
  -accel tcg \
  -machine q35 \
  -m 4G \
  -smp 4 \
  -drive file=/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2,format=qcow2,if=ide \
  -vga none \
  -device virtio-gpu-gl-pci,hostmem=1G,max_hostmem=1G,blob=true,neptune=true \
  -display cocoa
```

After installation, make sure that `DwmIsCompositionEnabled` returns true.
