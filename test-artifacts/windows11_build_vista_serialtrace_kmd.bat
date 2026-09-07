@echo off
setlocal
set "ROOT=C:\VistaSigning\VistaBuild\triton-kmd"
set "SRC=Y:\triton-kmd"
set "OUT=Z:\vista-signing-transfer"
set "WDKINC=C:\WinDDK\7600.16385.win7_wdk.100208-1538\inc"
set "VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

rem This directory is only a Windows build cache. Mirror each exact source
rem subtree so no stale KMD or VirtIO file can enter the signed package.
robocopy "%SRC%\VirtIO" "%ROOT%\VirtIO" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%SRC%\build" "%ROOT%\build" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%SRC%\viogpu\common" "%ROOT%\viogpu\common" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%SRC%\viogpu\shared" "%ROOT%\viogpu\shared" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%SRC%\viogpu\viogpu3d" "%ROOT%\viogpu\viogpu3d" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
copy /y "Y:\test-artifacts\viogpu3d-vista-serialtrace.vcxproj" "%ROOT%\viogpu\viogpu3d\viogpu3d.vcxproj" >nul
if errorlevel 1 exit /b 1

set "VISTA_D3D9_UMD=Y:\test-artifacts\vista-unified-umd\neptune_d3d9.dll"
set "VISTA_D3D9_WOW_UMD=Y:\test-artifacts\vista-unified-umd\neptune_d3d9_wow.dll"
if not exist "%VISTA_D3D9_UMD%" exit /b 1
if not exist "%VISTA_D3D9_WOW_UMD%" exit /b 1

call :build_x64
if errorlevel 1 exit /b 1
call :build_x86
if errorlevel 1 exit /b 1
exit /b 0

:build_x64
setlocal
call "%VCVARS%" x64
if errorlevel 1 (endlocal & exit /b 1)
set "CL=/D_AMD64_ /I%WDKINC%\ddk /I%WDKINC%\api /I%WDKINC%\crt"
MSBuild "%ROOT%\VirtIO\VirtioLib.vcxproj" /t:Clean;Build /p:Configuration="Vista x64";Platform=x64;PlatformToolset=v143 /m > "%OUT%\w11-vista-x64-virtiolib-build.log" 2>&1
if errorlevel 1 (endlocal & exit /b 1)
if not exist "%ROOT%\VirtIO\objfre_vista_amd64\amd64\virtiolib.lib" (endlocal & exit /b 1)
MSBuild "%ROOT%\viogpu\viogpu3d\viogpu3d.vcxproj" /t:Clean;ClCompile;ResourceCompile /p:Configuration="Vista x64";Platform=x64;VistaWdkPlatformToolset=v143 /m > "%OUT%\w11-vista-x64-kmd-build.log" 2>&1
if errorlevel 1 (endlocal & exit /b 1)
link @Y:\test-artifacts\viogpu3d-vista-x64.rsp >> "%OUT%\w11-vista-x64-kmd-build.log" 2>&1
if errorlevel 1 (endlocal & exit /b 1)
if not exist "%ROOT%\viogpu\viogpu3d\objfre_vista_amd64\amd64\viogpu3d.sys" (endlocal & exit /b 1)
copy /y "%ROOT%\viogpu\viogpu3d\objfre_vista_amd64\amd64\viogpu3d.sys" "%OUT%\viogpu3d-vista-x64.sys" >nul
if errorlevel 1 (endlocal & exit /b 1)
endlocal & exit /b 0

:build_x86
setlocal
call "%VCVARS%" x86
if errorlevel 1 (endlocal & exit /b 1)
set "CL=/D_X86_ /I%WDKINC%\ddk /I%WDKINC%\api /I%WDKINC%\crt"
MSBuild "%ROOT%\VirtIO\VirtioLib.vcxproj" /t:Clean;Build /p:Configuration="Vista x86";Platform=Win32;PlatformToolset=v143 /m > "%OUT%\w11-vista-x86-virtiolib-build.log" 2>&1
if errorlevel 1 (endlocal & exit /b 1)
if not exist "%ROOT%\VirtIO\objfre_vista_x86\i386\virtiolib.lib" (endlocal & exit /b 1)
MSBuild "%ROOT%\viogpu\viogpu3d\viogpu3d.vcxproj" /t:Clean;ClCompile;ResourceCompile /p:Configuration="Vista x86";Platform=Win32;VistaWdkPlatformToolset=v143 /m > "%OUT%\w11-vista-x86-kmd-build.log" 2>&1
if errorlevel 1 (endlocal & exit /b 1)
link @Y:\test-artifacts\viogpu3d-vista-x86.rsp >> "%OUT%\w11-vista-x86-kmd-build.log" 2>&1
if errorlevel 1 (endlocal & exit /b 1)
if not exist "%ROOT%\viogpu\viogpu3d\objfre_vista_x86\i386\viogpu3d.sys" (endlocal & exit /b 1)
copy /y "%ROOT%\viogpu\viogpu3d\objfre_vista_x86\i386\viogpu3d.sys" "%OUT%\viogpu3d-vista-x86.sys" >nul
if errorlevel 1 (endlocal & exit /b 1)
endlocal & exit /b 0
