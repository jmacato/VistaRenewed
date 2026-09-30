@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "OUT=C:\TritonSupermiumBridge\iexplore-baseline.txt"
del /q "%OUT%" 2>nul
echo IEXPLORE_PATH=C:\Program Files (x86)\Internet Explorer\iexplore.exe > "%OUT%"
C:\TritonSupermiumBridge\triton-ie7-mshtml-probe.exe >> "%OUT%" 2>&1
start "" /b "C:\Program Files (x86)\Internet Explorer\iexplore.exe" about:blank
timeout /t 8 /nobreak >nul
tasklist /fi "imagename eq iexplore.exe" /fo csv /nh >> "%OUT%" 2>&1
for /f "tokens=2 delims=," %%P in ('tasklist /fi "imagename eq iexplore.exe" /fo csv /nh ^| findstr /i "iexplore.exe"') do (
    C:\TritonSupermiumBridge\triton-ie7-mshtml-probe.exe %%P >> "%OUT%" 2>&1
)
reg query HKCR\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32 /ve >> "%OUT%" 2>&1
for /f "tokens=3" %%P in ('reg query HKCR\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32 /ve ^| findstr /i "REG_SZ"') do echo HTMLDOCUMENT_INPROC=%%P >> "%OUT%"
echo IEXPLORE MSHTML BASELINE CAPTURED >> "%OUT%"
type "%OUT%"
