<#
.SYNOPSIS
  khw-restart.ps1 - restart the 0404 devnode so the pending kernel-driver
  install starts without a full reboot, then show status and run emu-probe.
  Logged to package-ks\khw-restart.log.
#>
$ErrorActionPreference = 'Continue'
Start-Transcript -Path (Join-Path $PSScriptRoot 'package-ks\khw-restart.log') -Force

$dev = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'USB\VID_041E&PID_3F04*' } | Select-Object -First 1
if (-not $dev) {
    Write-Host "no present devnode - nothing to restart"
    Stop-Transcript
    exit 1
}
Write-Host "devnode: $($dev.InstanceId)  problem: $($dev.ConfigManagerErrorCode)"
pnputil /restart-device $dev.InstanceId

Start-Sleep -Seconds 5

$dev = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'USB\VID_041E&PID_3F04*' } | Select-Object -First 1
Write-Host "after restart:"
Write-Host "  status : $($dev.Status)  problem: $($dev.ConfigManagerErrorCode)"
Write-Host "  class  : $($dev.Class)  name: $($dev.FriendlyName)"
Write-Host "  service: $((Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Service').Data)"
Write-Host "  inf    : $((Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath').Data)"
Write-Host "  problem text: $($dev.ProblemText)"

$svc = (sc.exe query EmuUSBAudio | Out-String) -replace '\s+', ' '
Write-Host "service: $($svc.Trim())"

Write-Host "=== probe ==="
& (Join-Path $PSScriptRoot 'emuusbaudio\build\emu-probe.exe')

Stop-Transcript
