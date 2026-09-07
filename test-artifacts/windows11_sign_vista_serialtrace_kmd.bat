@echo off
setlocal
set "SIGNTOOL=Y:\driver\sdk\microsoft.windows.sdk.cpp\c\bin\10.0.28000.0\arm64\signtool.exe"
set "SIGNING_THUMBPRINT=2464DC7241B33AF0E6D333ED6D7542ADAD59DC1C"
set "OUT=Z:\vista-signing-transfer"
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%OUT%\viogpu3d-vista-x64.sys" > "%OUT%\w11-vista-kmd-sign.log" 2>&1
if errorlevel 1 exit /b 1
"%SIGNTOOL%" sign /fd SHA1 /sha1 %SIGNING_THUMBPRINT% /s My /sm "%OUT%\viogpu3d-vista-x86.sys" >> "%OUT%\w11-vista-kmd-sign.log" 2>&1
if errorlevel 1 exit /b 1
"%SIGNTOOL%" verify /pa /v "%OUT%\viogpu3d-vista-x64.sys" >> "%OUT%\w11-vista-kmd-sign.log" 2>&1
if errorlevel 1 exit /b 1
"%SIGNTOOL%" verify /pa /v "%OUT%\viogpu3d-vista-x86.sys" >> "%OUT%\w11-vista-kmd-sign.log" 2>&1
exit /b %errorlevel%
