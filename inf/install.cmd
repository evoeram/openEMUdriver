@echo off
rem openEMUdriver installer — run from an elevated prompt, from the inf\ folder.
rem SPDX-License-Identifier: LGPL-2.1-or-later
setlocal

set SDKBIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0
set SIGNTOOL=%SDKBIN%\App Certification Kit\signtool.exe
set INF2CAT=%SDKBIN%\x86\Inf2Cat.exe

echo == generating catalog ==
"%INF2CAT%" /driver:"%~dp0." /os:10_X64 || goto :err

echo == signing (self-signed "openEMUdriver Local" certificate must exist) ==
"%SIGNTOOL%" sign /sm /n "openEMUdriver Local" /fd SHA256 "%~dp0openemu.cat" || goto :err

echo == installing driver package ==
pnputil /add-driver "%~dp0openemu.inf" /install || goto :err

echo == restarting the E-MU device (replug USB if this step fails) ==
for /f "tokens=*" %%i in ('powershell -NoProfile -Command "(Get-PnpDevice | Where-Object { $_.InstanceId -like 'USB\VID_041E&PID_3F*' -and $_.InstanceId -notlike '*MI_*' } | Select-Object -First 1).InstanceId"') do set DEV=%%i
if defined DEV pnputil /restart-device "%DEV%"

echo == registering the ASIO driver ==
regsvr32 /s "%~dp0..\bin\openEMUdriver.asio" || goto :err

echo.
echo openEMUdriver installed. Run bin\emu-dump.exe to verify.
exit /b 0

:err
echo INSTALL FAILED
exit /b 1
