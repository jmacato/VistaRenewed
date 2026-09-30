@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=C:\TritonSupermiumBridge"
set "OUT=%ROOT%\iexplore-shim.txt"
set "LOG=%ROOT%\iexplore-mshtml-activation-probe.log"
set "LOWLOG=%USERPROFILE%\AppData\LocalLow\TritonSupermiumBridge\iexplore-mshtml-activation-probe.log"
set "USERKEY=HKCU\Software\Classes\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32"
set "STOCKKEY=HKCR\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32"
set "EXISTING="
del /q "%OUT%" "%LOG%" 2>nul
del /q "%LOWLOG%" 2>nul
for /f "tokens=3" %%P in ('reg query "%USERKEY%" /ve /reg:32 2^>nul ^| findstr /i "REG_SZ"') do set "EXISTING=%%P"
for /f "tokens=3" %%P in ('reg query "%STOCKKEY%" /ve /reg:32 ^| findstr /i "REG_SZ"') do set "STOCK=%%P"
if not defined STOCK (
    echo IEXPLORE_SHIM_MODE=stock-query-failed > "%OUT%"
    echo IEXPLORE SHIM EXERCISE CAPTURED >> "%OUT%"
    type "%OUT%"
    exit /b 1
)
echo IEXPLORE_SHIM_MODE=per-user-activation-probe > "%OUT%"
echo STOCK_HTMLDOCUMENT_INPROC=!STOCK! >> "%OUT%"
echo ACTIVEX_POLICY=unsupported-stubbed >> "%OUT%"
if defined EXISTING echo PREVIOUS_USER_HTMLDOCUMENT_INPROC=!EXISTING! >> "%OUT%"
reg add "%USERKEY%" /ve /t REG_SZ /d "%ROOT%\triton-ie7-mshtml-activation-probe.dll" /f /reg:32 >> "%OUT%" 2>&1
if errorlevel 1 goto :restore
reg query "%STOCKKEY%" /ve /reg:32 >> "%OUT%" 2>&1
set "TESTURL=file:///D:/visible-supermium.html"
if /i "%~1"=="input" set "TESTURL=file:///C:/TritonSupermiumBridge/ie-input.html"
if /i "%~1"=="web" set "TESTURL=https://example.com/"
if /i "%~1"=="http" set "TESTURL=http://10.0.2.2:18765/engine-probe"
set "WAIT_SECONDS=12"
if /i "%~2"=="extended" set "WAIT_SECONDS=30"
start "" /b "C:\Program Files (x86)\Internet Explorer\iexplore.exe" "!TESTURL!"
timeout /t !WAIT_SECONDS! /nobreak >nul
if exist "%ROOT%\ie-window-health.exe" "%ROOT%\ie-window-health.exe" >> "%OUT%" 2>&1
tasklist /fi "imagename eq iexplore.exe" /fo csv /nh >> "%OUT%" 2>&1
tasklist /fi "imagename eq chrome.exe" /fo csv /nh >> "%OUT%" 2>&1
for /f "tokens=2 delims=," %%P in ('tasklist /fi "imagename eq iexplore.exe" /fo csv /nh ^| findstr /i "iexplore.exe"') do (
    C:\TritonSupermiumBridge\triton-ie7-mshtml-probe.exe %%P >> "%OUT%" 2>&1
)
:restore
if defined EXISTING (
    reg add "%USERKEY%" /ve /t REG_SZ /d "!EXISTING!" /f /reg:32 >> "%OUT%" 2>&1
) else (
    reg delete "%USERKEY%" /f /reg:32 >> "%OUT%" 2>&1
)
set "RESTORED="
for /f "tokens=3" %%P in ('reg query "%STOCKKEY%" /ve /reg:32 ^| findstr /i "REG_SZ"') do set "RESTORED=%%P"
echo RESTORED_HTMLDOCUMENT_INPROC=!RESTORED! >> "%OUT%"
if exist "%LOG%" type "%LOG%" >> "%OUT%"
if exist "%LOWLOG%" type "%LOWLOG%" >> "%OUT%"
echo IEXPLORE SHIM EXERCISE CAPTURED >> "%OUT%"
type "%OUT%"
if /i not "!RESTORED!"=="!STOCK!" exit /b 1
exit /b 0
