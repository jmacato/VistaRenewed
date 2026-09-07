@echo off
setlocal EnableExtensions

rem Build catalog-consistent Vista x64 and x86 packages from the two binaries
rem produced by windows11_build_vista_serialtrace_kmd.bat.
set "INF2CAT=Y:\driver\sdk\microsoft.windows.wdk.x64\c\bin\10.0.28000.0\x86\Inf2Cat.exe"
set "SIGNTOOL=Y:\driver\sdk\microsoft.windows.sdk.cpp\c\bin\10.0.28000.0\arm64\signtool.exe"
set "SIGNING_THUMBPRINT=2464DC7241B33AF0E6D333ED6D7542ADAD59DC1C"
set "OUT=Z:\vista-signing-transfer"
set "PKG_X64_NAME=%~1"
if "%PKG_X64_NAME%"=="" set "PKG_X64_NAME=vista-driver-x64-pnp-current"
set "PKG_X86_NAME=%~2"
if "%PKG_X86_NAME%"=="" set "PKG_X86_NAME=vista-driver-x86-pnp-current"
set "DRIVER_DATE=%~3"
set "DRIVER_VERSION=%~4"
set "FINAL_X64=%OUT%\%PKG_X64_NAME%"
set "FINAL_X86=%OUT%\%PKG_X86_NAME%"
set "PKG_X64=%FINAL_X64%.building"
set "PKG_X86=%FINAL_X86%.building"
set "INF_X64=Y:\test-artifacts\vista-driver-x64-kd-serialtrace\viogpu3d-diagnostic.inf"
set "INF_X86=Y:\triton-kmd\viogpu\viogpu3d\viogpu3d_vista_x86.inx"
set "UMD_SOURCE=Y:\test-artifacts\vista-unified-umd"
set "DEPLOY_SERVICE=Y:\test-artifacts\vista-deploy-service\triton-vista-deploy.exe"
set "PROBE_X64=%OUT%\triton9_runtime_probe_x64.exe"

if exist "%PKG_X64%" rmdir /s /q "%PKG_X64%"
if exist "%PKG_X64%" exit /b 1
if exist "%PKG_X86%" rmdir /s /q "%PKG_X86%"
if exist "%PKG_X86%" exit /b 1
md "%PKG_X64%" || exit /b 1
md "%PKG_X86%" || exit /b 1

copy /y "%OUT%\viogpu3d-vista-x64.sys" "%PKG_X64%\viogpu3d.sys" >nul || exit /b 1
copy /y "%INF_X64%" "%PKG_X64%\viogpu3d-diagnostic.inf" >nul || exit /b 1
copy /y "%UMD_SOURCE%\neptune_d3d9.dll" "%PKG_X64%\neptune_d3d9.dll" >nul || exit /b 1
copy /y "%UMD_SOURCE%\neptune_d3d9_wow.dll" "%PKG_X64%\neptune_d3d9_wow.dll" >nul || exit /b 1
copy /y "%DEPLOY_SERVICE%" "%PKG_X64%\triton-vista-deploy.exe" >nul || exit /b 1
copy /y "%PROBE_X64%" "%PKG_X64%\triton9_runtime_probe_x64.exe" >nul || exit /b 1

copy /y "%OUT%\viogpu3d-vista-x86.sys" "%PKG_X86%\viogpu3d.sys" >nul || exit /b 1
copy /y "%INF_X86%" "%PKG_X86%\viogpu3d.inf" >nul || exit /b 1
copy /y "%UMD_SOURCE%\neptune_d3d9_wow.dll" "%PKG_X86%\neptune_d3d9.dll" >nul || exit /b 1

if not "%DRIVER_DATE%"=="" if not "%DRIVER_VERSION%"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$p='%PKG_X64%\viogpu3d-diagnostic.inf'; $s=[IO.File]::ReadAllText($p); $s=[Text.RegularExpressions.Regex]::Replace($s,'(?m)^DriverVer\s*=.*$','DriverVer = %DRIVER_DATE%,%DRIVER_VERSION%'); [IO.File]::WriteAllText($p,$s,[Text.Encoding]::ASCII)" || exit /b 1
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$p='%PKG_X86%\viogpu3d.inf'; $s=[IO.File]::ReadAllText($p); $s=[Text.RegularExpressions.Regex]::Replace($s,'(?m)^DriverVer\s*=.*$','DriverVer = %DRIVER_DATE%,%DRIVER_VERSION%'); [IO.File]::WriteAllText($p,$s,[Text.Encoding]::ASCII)" || exit /b 1
)

if not exist "%INF2CAT%" exit /b 1
if not exist "%SIGNTOOL%" exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X64%\neptune_d3d9.dll" || exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X64%\neptune_d3d9_wow.dll" || exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X64%\triton-vista-deploy.exe" || exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X64%\triton9_runtime_probe_x64.exe" || exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X86%\neptune_d3d9.dll" || exit /b 1
"%INF2CAT%" /driver:"%PKG_X64%" /os:Vista_X64 || exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X64%\viogpu3d-vista-x64.cat" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X64%\viogpu3d-vista-x64.cat" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X64%\viogpu3d.sys" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X64%\neptune_d3d9.dll" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X64%\neptune_d3d9_wow.dll" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X64%\triton-vista-deploy.exe" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X64%\triton9_runtime_probe_x64.exe" || exit /b 1
"%INF2CAT%" /driver:"%PKG_X86%" /os:Vista_X86 || exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%PKG_X86%\viogpu3d-vista-x86.cat" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X86%\viogpu3d-vista-x86.cat" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X86%\viogpu3d.sys" || exit /b 1
"%SIGNTOOL%" verify /pa /v "%PKG_X86%\neptune_d3d9.dll" || exit /b 1

certutil -hashfile "%PKG_X64%\viogpu3d.sys" SHA1
certutil -hashfile "%PKG_X64%\viogpu3d-vista-x64.cat" SHA1
certutil -hashfile "%PKG_X86%\viogpu3d.sys" SHA1
certutil -hashfile "%PKG_X86%\viogpu3d-vista-x86.cat" SHA1
if exist "%FINAL_X64%" rmdir /s /q "%FINAL_X64%"
if exist "%FINAL_X64%" exit /b 1
if exist "%FINAL_X86%" rmdir /s /q "%FINAL_X86%"
if exist "%FINAL_X86%" exit /b 1
move "%PKG_X64%" "%FINAL_X64%" >nul || exit /b 1
move "%PKG_X86%" "%FINAL_X86%" >nul || exit /b 1
exit /b 0
