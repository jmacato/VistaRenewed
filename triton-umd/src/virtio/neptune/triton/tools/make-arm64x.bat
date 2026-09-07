@echo off
setlocal enableextensions enabledelayedexpansion
rem ===========================================================================
rem  make-arm64x.bat - build the ARM64X forwarder for the Triton UMD.
rem
rem  Windows cannot load a single DLL that satisfies both native-ARM64 and
rem  ARM64EC (incl. x64-emulated) callers, so Triton is built twice and stitched
rem  together behind a code-less ARM64X "forwarder" DLL.  The forwarder exports
rem  the same symbols as the real driver, but every export is a forwarder string
rem  resolved per-caller by the loader:
rem
rem     ARM64 native view (/defArm64Native) --> neptune_umd_arm64.dll  (arm64)
rem     EC view           (/def)            --> neptune_umd_ec.dll     (arm64ec)
rem
rem  The two inputs are the arm64 and arm64ec builds (NOT x64 -- an arm64ec image
rem  reports machine type 8664/x64 because it uses the x64 ABI, but it is arm64ec
rem  code and also services x64-emulated callers through the EC view).
rem
rem  Which physical input feeds which view is decided by the DLL's machine type,
rem  NOT by argument order -- pass the two builds in either order:
rem     arm64    (dumpbin: "AA64 machine (ARM64)")           --> native view
rem     arm64ec  (dumpbin: "8664 machine (x64) (ARM64X)")    --> EC view
rem
rem  The deliverable is THREE files that must ship side by side:
rem     neptune_umd.dll        <- the ARM64X forwarder (what the OS loads)
rem     neptune_umd_arm64.dll  <- the arm64 build, renamed
rem     neptune_umd_ec.dll     <- the arm64ec build, renamed
rem
rem  Usage:
rem     make-arm64x.bat <dll_a> <dll_b> [output_dir]
rem
rem     <dll_a> <dll_b>  the two neptune_umd.dll builds, in either order
rem                      (one arm64, one arm64ec)
rem     [output_dir]     where the three files are written (default: .\arm64x)
rem
rem  The export list is taken from neptune_umd.def (one directory up), which is
rem  the same file meson feeds to the real builds, so the forwarder can never
rem  drift from the driver's ABI.
rem ===========================================================================

rem --- Names the forwarder points at (also the on-disk payload filenames). ----
set "BASENAME=neptune_umd"
set "NATIVE_MODULE=neptune_umd_arm64"
set "EC_MODULE=neptune_umd_ec"

rem --- Resolve locations relative to this script. -----------------------------
set "SCRIPT_DIR=%~dp0"
set "SRC_DEF=%SCRIPT_DIR%..\neptune_umd.def"

rem --- Parse arguments. -------------------------------------------------------
if "%~1"=="" goto :usage
if "%~2"=="" goto :usage
set "DLL_A=%~f1"
set "DLL_B=%~f2"
if "%~3"=="" (
  set "OUT_DIR=%CD%\arm64x"
) else (
  set "OUT_DIR=%~f3"
)

if not exist "%DLL_A%"   ( echo [error] input DLL not found: %DLL_A%& exit /b 1 )
if not exist "%DLL_B%"   ( echo [error] input DLL not found: %DLL_B%& exit /b 1 )
if not exist "%SRC_DEF%" ( echo [error] export def not found: %SRC_DEF%& exit /b 1 )

rem --- Locate VsDevCmd.bat via vswhere. ---------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere.exe not found; is Visual Studio installed?
  exit /b 1
)

set "VSINSTALL="
rem Prefer an install that actually carries the ARM64 build tools.
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath 2^>nul`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -property installationPath 2^>nul`) do set "VSINSTALL=%%I"
)
if not defined VSINSTALL (
  echo [error] no Visual Studio installation found via vswhere.
  exit /b 1
)
set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo [error] VsDevCmd.bat not found under: %VSINSTALL%
  exit /b 1
)

rem --- Pick host tools matching the machine we're running on. -----------------
set "HOST_ARCH=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST_ARCH=arm64"
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "HOST_ARCH=arm64"

echo [info] Visual Studio : %VSINSTALL%
echo [info] host/target    : %HOST_ARCH% -^> arm64x
echo [info] output dir     : %OUT_DIR%

rem --- Enter the arm64-targeting developer environment. -----------------------
rem  Put the Installer dir on PATH so VsDevCmd's own vswhere lookup stays quiet.
for %%V in ("%VSWHERE%") do set "PATH=%%~dpV;%PATH%"
call "%VSDEVCMD%" -arch=arm64 -host_arch=%HOST_ARCH% -startdir=none -no_logo >nul
if errorlevel 1 (
  echo [error] VsDevCmd.bat failed to initialize the arm64 environment.
  exit /b 1
)

rem --- Route each input to a view by its machine type, not by argument order. --
rem  arm64   -> "AA64 machine (ARM64)"        -> native view (neptune_umd_arm64)
rem  arm64ec -> "8664 machine (x64) (ARM64X)" -> EC view     (neptune_umd_ec)
set "NATIVE_INPUT="
set "EC_INPUT="
call :classify "%DLL_A%" ROLE_A
call :classify "%DLL_B%" ROLE_B
echo [info] %DLL_A% -^> %ROLE_A%
echo [info] %DLL_B% -^> %ROLE_B%
if "%ROLE_A%"=="native" if "%ROLE_B%"=="ec"     ( set "NATIVE_INPUT=%DLL_A%" & set "EC_INPUT=%DLL_B%" )
if "%ROLE_A%"=="ec"     if "%ROLE_B%"=="native" ( set "NATIVE_INPUT=%DLL_B%" & set "EC_INPUT=%DLL_A%" )
if not defined NATIVE_INPUT (
  echo [error] expected one arm64 input and one arm64ec input; got '%ROLE_A%' and '%ROLE_B%'.
  echo [error] each input must be a distinct machine type ^(AA64 vs 8664^).
  exit /b 1
)

rem --- Scratch workspace (cleaned up on exit). --------------------------------
set "WORK=%TEMP%\triton-arm64x-%RANDOM%%RANDOM%"
mkdir "%WORK%" 2>nul
if not exist "%WORK%" ( echo [error] could not create work dir: %WORK%& exit /b 1 )

set "NAT_DEF=%WORK%\forward_native.def"
set "EC_DEF=%WORK%\forward_ec.def"

rem --- Generate the two forwarder .def files from neptune_umd.def. -------------
rem  Everything after the EXPORTS line is an export name (first token); we skip
rem  LIBRARY/comment/blank lines and rewrite each name as a forwarder.
> "%NAT_DEF%" echo EXPORTS
> "%EC_DEF%"  echo EXPORTS
set "IN_EXPORTS="
set "EXPORT_COUNT=0"
for /f "usebackq delims=" %%L in ("%SRC_DEF%") do (
  set "RAW=%%L"
  set "TOK="
  for /f "tokens=1" %%A in ("!RAW!") do set "TOK=%%A"
  if defined TOK (
    set "FIRST=!TOK:~0,1!"
    if /i "!TOK!"=="EXPORTS" (
      set "IN_EXPORTS=1"
    ) else if "!FIRST!"==";" (
      rem comment - skip
    ) else if /i "!TOK!"=="LIBRARY" (
      rem module header - skip
    ) else if defined IN_EXPORTS (
      >> "%NAT_DEF%" echo     !TOK! = %NATIVE_MODULE%.!TOK!
      >> "%EC_DEF%"  echo     !TOK! = %EC_MODULE%.!TOK!
      set /a EXPORT_COUNT+=1
    )
  )
)
if "%EXPORT_COUNT%"=="0" (
  echo [error] no exports parsed from %SRC_DEF%
  goto :fail
)
echo [info] forwarding %EXPORT_COUNT% export(s).

rem --- Minimal object content for each view (forwarders carry no real code). --
> "%WORK%\empty.cpp" echo void __triton_arm64x_stub(void){}
pushd "%WORK%"

cl /nologo /c /Foempty_arm64.obj empty.cpp
if errorlevel 1 ( echo [error] cl failed building the arm64 stub object.& popd & goto :fail )
cl /nologo /c /arm64EC /Foempty_ec.obj empty.cpp
if errorlevel 1 ( echo [error] cl failed building the arm64ec stub object.& popd & goto :fail )

rem --- Import libs materializing each view's forwarder exports. ----------------
lib /nologo /machine:arm64 /def:"%NAT_DEF%" /out:forward_native.lib
if errorlevel 1 ( echo [error] lib failed for the native view.& popd & goto :fail )
lib /nologo /machine:x64 /def:"%EC_DEF%" /out:forward_ec.lib
if errorlevel 1 ( echo [error] lib failed for the EC view.& popd & goto :fail )

rem --- Link the ARM64X forwarder DLL. -----------------------------------------
link /nologo /dll /noentry /machine:arm64x ^
  /defArm64Native:"%NAT_DEF%" /def:"%EC_DEF%" ^
  empty_arm64.obj empty_ec.obj ^
  /out:%BASENAME%.dll forward_native.lib forward_ec.lib
if errorlevel 1 ( echo [error] link failed producing the arm64x forwarder.& popd & goto :fail )
popd

rem --- Assemble the deliverable. ----------------------------------------------
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
copy /y "%WORK%\%BASENAME%.dll" "%OUT_DIR%\%BASENAME%.dll" >nul
copy /y "%NATIVE_INPUT%"        "%OUT_DIR%\%NATIVE_MODULE%.dll" >nul
copy /y "%EC_INPUT%"            "%OUT_DIR%\%EC_MODULE%.dll" >nul

echo.
echo [ok] wrote:
echo     %OUT_DIR%\%BASENAME%.dll        (arm64x forwarder)
echo     %OUT_DIR%\%NATIVE_MODULE%.dll  (arm64 payload)
echo     %OUT_DIR%\%EC_MODULE%.dll      (arm64ec payload)
echo.

rem --- Sanity checks. ---------------------------------------------------------
echo [verify] machine type:
dumpbin /nologo /headers "%OUT_DIR%\%BASENAME%.dll" | findstr /i "machine"
echo [verify] forwarded exports:
dumpbin /nologo /exports "%OUT_DIR%\%BASENAME%.dll" | findstr /i "forwarded"

rd /s /q "%WORK%" 2>nul
endlocal
exit /b 0

:fail
rd /s /q "%WORK%" 2>nul
endlocal
exit /b 1

:usage
echo Usage: %~nx0 ^<dll_a^> ^<dll_b^> [output_dir]
echo.
echo   ^<dll_a^> ^<dll_b^>  the two neptune_umd.dll builds, in either order
echo                    (one arm64, one arm64ec)
echo   [output_dir]     destination for the 3-file set (default: .\arm64x)
endlocal
exit /b 2

rem ---------------------------------------------------------------------------
rem  :classify <dll> <out_var> - set <out_var> to the input's forwarder role:
rem    native  = classic ARM64        ("AA64 machine (ARM64)")
rem    ec      = ARM64EC / x64         ("8664 machine (x64) ...")
rem    arm64x  = already a hybrid image (AA64 + ARM64X) -> invalid input
rem    unknown = machine type not recognized
rem ---------------------------------------------------------------------------
:classify
setlocal enabledelayedexpansion
set "_line="
for /f "usebackq delims=" %%H in (`dumpbin /nologo /headers "%~1" 2^>nul ^| findstr /i "machine"`) do set "_line=%%H"
set "_role=unknown"
echo(!_line! | findstr /i "8664" >nul
if not errorlevel 1 (
  set "_role=ec"
) else (
  echo(!_line! | findstr /i "AA64" >nul
  if not errorlevel 1 (
    echo(!_line! | findstr /i "ARM64X" >nul
    if not errorlevel 1 ( set "_role=arm64x" ) else ( set "_role=native" )
  )
)
endlocal & set "%~2=%_role%"
exit /b
