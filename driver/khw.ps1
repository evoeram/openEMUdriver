<#
.SYNOPSIS
  khw.ps1 - one elevated hardware run for the M2 kernel driver.

  1. installs the kernel driver package (kinstall.ps1; drops the v1 WinUSB
     packages so PnP rebinds the 0404 to EmuUSBAudio)
  2. sets Parameters\TraceLevel = 5 in the service key (descriptor hex dump)
  3. sets the Debug Print Filter IHVDRIVER mask so DbgPrintEx output reaches
     DebugView (takes effect after the next reboot)
  4. installs DebugView (Sysinternals) for that trace
  5. runs emu-probe with retries until the devnode has finished starting

  Everything lands in package-ks\khw-run.log. -Uninstall hands back to kinstall.
#>
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'package-ks\khw-run.log'
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'package-ks') | Out-Null
Start-Transcript -Path $log -Force

Write-Host "=== 1. kernel driver install (kinstall.ps1) ==="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'kinstall.ps1')

Write-Host "=== 2. trace configuration ==="
$svcKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\EmuUSBAudio\Parameters'
New-Item -Path $svcKey -Force -ErrorAction SilentlyContinue | Out-Null
Set-ItemProperty -Path $svcKey -Name TraceLevel -Value 5 -Type DWord
Write-Host "TraceLevel = 5 (service key)"

$dpfKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter'
New-Item -Path $dpfKey -Force | Out-Null
Set-ItemProperty -Path $dpfKey -Name IHVDRIVER -Value 15 -Type DWord
Write-Host "Debug Print Filter IHVDRIVER = 15 (active after next reboot)"

Write-Host "=== 3. DebugView (Sysinternals) ==="
winget install --id Microsoft.Sysinternals.DebugView --silent `
    --accept-source-agreements --accept-package-agreements
Write-Host "winget exit: $LASTEXITCODE"

Write-Host "=== 4. control-interface probe (retries while the devnode starts) ==="
$probe = Join-Path $PSScriptRoot 'emuusbaudio\build\emu-probe.exe'
$probeResult = 1
for ($i = 1; $i -le 4; $i++) {
    Write-Host "--- probe attempt $i ---"
    & $probe
    $probeResult = $LASTEXITCODE
    if ($probeResult -eq 0) { break }
    Start-Sleep -Seconds 4
}

Write-Host "=== 5. control interface registration in the registry ==="
$cls = Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{6DC68D18-D4C8-4DAC-A456-6CA921953BE8}' `
        -ErrorAction SilentlyContinue
if ($cls) {
    foreach ($c in $cls) {
        $enabled = (Get-ItemProperty -Path $c.PSPath -Name '(default)' -ErrorAction SilentlyContinue).'(default)'
        Write-Host "link  : $($c.PSChildName)"
        Write-Host "enabled: $enabled"
    }
} else {
    Write-Host "no DeviceClasses entry for the control interface GUID"
}

Stop-Transcript
