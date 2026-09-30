@echo off
setlocal EnableExtensions
set "OUT=C:\TritonSupermiumBridge\supermium-visible-capture.txt"
set "PROFILE=C:\TritonSupermiumBridge\VisibleCaptureProfile"
del /q "%OUT%" 2>nul
if not exist "%PROFILE%" md "%PROFILE%"
start "Supermium Vista renderer" "C:\TritonSupermium\chrome.exe" --new-window --no-first-run --disable-gpu --disable-background-networking --disable-component-update --disable-default-apps --user-data-dir="%PROFILE%" "file:///D:/visible-supermium.html"
timeout /t 20 /nobreak >nul
tasklist /fi "imagename eq chrome.exe" /fo csv /nh > "%OUT%" 2>&1
type "%OUT%"
exit /b 0
