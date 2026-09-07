@echo off
setlocal EnableExtensions

set "PACKAGE=%~1"
if "%PACKAGE%"=="" set "PACKAGE=vista-driver-x64-pnp-current"
set "LOCAL_ROOT=%~2"
set "BASE_URL=http://10.0.2.2:8089/%PACKAGE%"
set "DEST=C:\pnpserial7"
set "LOG=%DEST%\deploy.log"

if not exist "%DEST%" md "%DEST%"
if errorlevel 1 exit /b 10

>"%LOG%" echo [%DATE% %TIME%] Deploying %PACKAGE%

call :download viogpu3d-diagnostic.inf || exit /b 20
call :download viogpu3d-vista-x64.cat || exit /b 21
call :download viogpu3d.sys || exit /b 22
call :download neptune_d3d9.dll || exit /b 23
call :download neptune_d3d9_wow.dll || exit /b 24

>>"%LOG%" 2>&1 pnputil -a "%DEST%\viogpu3d-diagnostic.inf"
if errorlevel 1 exit /b 30

>>"%LOG%" 2>&1 C:\devcon.exe update "%DEST%\viogpu3d-diagnostic.inf" "PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01"
set "DEVCON_STATUS=%ERRORLEVEL%"
if %DEVCON_STATUS% GEQ 2 exit /b 31

>>"%LOG%" 2>&1 fc /b C:\Windows\System32\drivers\viogpu3d.sys "%DEST%\viogpu3d.sys"
if errorlevel 1 exit /b 32
>>"%LOG%" 2>&1 fc /b C:\Windows\System32\neptune_d3d9.dll "%DEST%\neptune_d3d9.dll"
if errorlevel 1 exit /b 33
>>"%LOG%" 2>&1 fc /b C:\Windows\SysWOW64\neptune_d3d9_wow.dll "%DEST%\neptune_d3d9_wow.dll"
if errorlevel 1 exit /b 34

reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" /s /v UserModeDriverName 2>nul | find "neptune_d3d9.dll" >nul
if errorlevel 1 exit /b 35
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" /s /v UserModeDriverNameWow 2>nul | find "neptune_d3d9_wow.dll" >nul
if errorlevel 1 exit /b 36

>>"%LOG%" 2>&1 certutil -hashfile C:\Windows\System32\drivers\viogpu3d.sys
>>"%LOG%" 2>&1 certutil -hashfile C:\Windows\System32\neptune_d3d9.dll
>>"%LOG%" 2>&1 certutil -hashfile C:\Windows\SysWOW64\neptune_d3d9_wow.dll
rem Keep checked Vista attached to KD while the WDDM 1.0 port is under test.
rem Its noncritical DDM checks identify callback contract violations before
rem they degrade into a black display and an unexplained ResetDevice call.
>>"%LOG%" 2>&1 bcdedit /debug on
if errorlevel 1 exit /b 37
rem The host deployment script owns reboot/KD sequencing.  Also remove a
rem persistent Safe Mode setting if the host needed it to obtain networking.
>>"%LOG%" 2>&1 bcdedit /deletevalue {current} safeboot
>>"%LOG%" echo [%DATE% %TIME%] Deployment verified; ready for host reboot.
if not "%LOCAL_ROOT%"=="" (
    echo OKINSTALLREADYOK
    exit /b 0
)
>>"%LOG%" 2>&1 cscript //nologo "%DEST%\httpget.vbs" "http://10.0.2.2:8089/__vista_install_complete__" "%DEST%\install-complete.txt"
if errorlevel 1 exit /b 38
shutdown /s /f /t 5
exit /b 0

:download
del /f /q "%DEST%\%~1" >nul 2>&1
if not "%LOCAL_ROOT%"=="" (
    >>"%LOG%" 2>&1 copy /y "%LOCAL_ROOT%\%PACKAGE%\%~1" "%DEST%\%~1"
    if errorlevel 1 exit /b 1
    exit /b 0
)
>>"%LOG%" 2>&1 cscript //nologo "%DEST%\httpget.vbs" "%BASE_URL%/%~1" "%DEST%\%~1"
exit /b %ERRORLEVEL%
