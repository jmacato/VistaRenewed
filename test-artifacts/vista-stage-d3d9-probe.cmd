@echo off
setlocal
set "SOURCE=%~dp0"
set "DEST=C:\Windows\Temp"

copy /y "%SOURCE%triton9_runtime_probe_x64.exe" "%DEST%\triton9_runtime_probe_x64.exe" >nul || exit /b 10
del /q "%DEST%\triton9-probe.log" 2>nul
del /q "%DEST%\triton9-ddi.log" 2>nul
del /q "%DEST%\triton9-service.log" 2>nul
del /q "C:\triton9-ddi.log" 2>nul
rem Remove the abandoned Task Scheduler variants.  A checked Vista taskeng
rem assertion in either stale task can stop the whole kernel-debugged guest.
schtasks /delete /tn TritonD3D9Probe /f >nul 2>nul
schtasks /delete /tn TritonD3D9ProbeTask /f >nul 2>nul
schtasks /delete /tn Triton9RuntimeProbe /f >nul 2>nul
sc stop TritonD3D9Probe >nul 2>nul
sc delete TritonD3D9Probe >nul 2>nul
sc stop TritonD3D9ProbeV2 >nul 2>nul
sc delete TritonD3D9ProbeV2 >nul 2>nul
sc create TritonD3D9ProbeV2 binPath= C:\Windows\Temp\triton9_runtime_probe_x64.exe start= auto type= own error= normal obj= LocalSystem DisplayName= "Triton D3D9 bring-up probe V2" >"%DEST%\triton9-service-stage.log" 2>&1 || goto service_create_failed
sc qc TritonD3D9ProbeV2 >>"%DEST%\triton9-service-stage.log" 2>&1
echo PROBEREADY
exit /b 0

:service_create_failed
type "%DEST%\triton9-service-stage.log"
echo OKPROBEFAIL12OK
exit /b 12
