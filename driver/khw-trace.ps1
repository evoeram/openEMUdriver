<#
.SYNOPSIS
  khw-trace.ps1 - capture the driver's DbgPrintEx output around a devnode
  restart, then run emu-probe. One elevated run, logged to khw-trace.log.

  DebugView starts with kernel capture logging to package-ks\dbgtrace.log,
  the 0404 devnode is restarted so StartDevice runs again, the log is read
  back and the probe checks the control interface.
#>
$ErrorActionPreference = 'Continue'
Start-Transcript -Path (Join-Path $PSScriptRoot 'package-ks\khw-trace.log') -Force

# Correct the DPFLTR mask for the NEXT boot: DbgPrintEx tests mask & (1<<Level),
# the driver's levels are 2..5, so the mask must include bits 4|8|16|32 = 63.
# The registry value is read at boot; this boot keeps running with 15 (ERROR/WARN).
$dpfKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter'
Set-ItemProperty -Path $dpfKey -Name IHVDRIVER -Value 63 -Type DWord
Write-Host "IHVDRIVER set to 63 for the next boot (this boot runs with 15)"

$dev = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'USB\VID_041E&PID_3F04*' } | Select-Object -First 1
if (-not $dev) {
    Write-Host "no present devnode"
    Stop-Transcript
    exit 1
}
Write-Host "before restart: status $($dev.Status), problem $($dev.ConfigManagerErrorCode)"

$trace = Join-Path $PSScriptRoot 'package-ks\dbgtrace.log'
Remove-Item $trace -Force -ErrorAction SilentlyContinue

$dv = "$env:LOCALAPPDATA\Microsoft\WinGet\Links\dbgview.exe"
Write-Host "starting DebugView (kernel capture, log $trace)"
Start-Process $dv -ArgumentList @('/accepteula','/k','/l',"$trace")
Start-Sleep -Seconds 4
Write-Host "log present after DebugView start: $(Test-Path $trace)"

Write-Host "=== restart device ==="
pnputil /restart-device $dev.InstanceId
Start-Sleep -Seconds 10

$dev = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'USB\VID_041E&PID_3F04*' } | Select-Object -First 1
Write-Host "after restart: status $($dev.Status), problem $($dev.ConfigManagerErrorCode)"
Write-Host "service state: $((sc.exe query EmuUSBAudio | Out-String))"

Stop-Process -Name DbgView -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "=== kernel debug trace ==="
if (Test-Path $trace) {
    Get-Content $trace
} else {
    Write-Host "dbgtrace.log was not created (DebugView /l switch unsupported?)"
}

Write-Host "=== probe ==="
& (Join-Path $PSScriptRoot 'emuusbaudio\build\emu-probe.exe')

Stop-Transcript
