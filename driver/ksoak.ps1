<#
.SYNOPSIS
  ksoak.ps1 [-Cycles 50] [-RemoveCycles 10] [-Verifier] [-VerifierOff] [-Quiet]

.DESCRIPTION
  M1 acceptance harness for the PortCls PnP skeleton: repeatedly disable/enable
  and remove/re-add the 0404 devnode while watching nonpaged pool and the System
  event log. A leak, a stuck remove lock or a bad DeviceExtension shows up as a
  hung cycle (disable/enable stops responding), a growing pool trend or a BSOD.

  -Verifier  enables Driver Verifier for emuusbaudio.sys. It takes effect only
             after a reboot, which this script does NOT perform - it prints the
             command instead of running it. Never do this on a machine you
             cannot recover with a kernel debugger or Safe Mode.
  -VerifierOff  runs 'verifier /reset' (still needs a reboot to take effect).

  Run from an elevated PowerShell on the test machine, with the 0404 plugged in
  and kinstall.ps1 already done.

.EXAMPLE
  ./ksoak.ps1 -Cycles 50 -RemoveCycles 10
  ./ksoak.ps1 -Verifier          # then reboot manually, then rerun the soak
#>
[CmdletBinding()]
param(
    [int]$Cycles = 50,
    [int]$RemoveCycles = 10,
    [switch]$Verifier,
    [switch]$VerifierOff,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-EmuDevnode {
    Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'USB\VID_041E&PID_3F04' } | Select-Object -First 1
}

# WMI counter names are English on every locale, unlike Get-Counter paths.
function Get-PoolNonpaged {
    (Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory).PoolNonpagedBytes
}

function Get-EventBaseline {
    (Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddHours(-4) } `
        -ErrorAction SilentlyContinue | Measure-Object).Count
}

# CM_PROB_DISABLED: the reliable 'it is disabled' signal, because Get-PnpDevice
# reports Status 'Unknown' for every problem, not just this one.
$script:CM_PROB_DISABLED = 22

function Wait-Disabled([string]$InstanceId) {
    for ($i = 0; $i -lt 40; $i++) {
        $d = Get-PnpDevice -InstanceId $InstanceId -ErrorAction SilentlyContinue
        if ($d -and $d.ConfigManagerErrorCode -eq $CM_PROB_DISABLED) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Wait-Ok([string]$InstanceId) {
    for ($i = 0; $i -lt 40; $i++) {
        $d = Get-PnpDevice -InstanceId $InstanceId -ErrorAction SilentlyContinue
        if ($d -and $d.Status -eq 'OK') { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

if (-not (Test-Admin)) { throw "needs an elevated shell (pnputil /disable-device, verifier)." }

if ($Verifier) {
    Write-Host @"
Driver Verifier for emuusbaudio.sys. This is the M1 acceptance condition, but it
only arms after a reboot and a verified driver that bugs will BSOD the machine.
Requirements before you run it:
  - a test machine you can boot with a kernel debugger (or Safe Mode / last
    known good configuration),
  - bcdedit /set testsigning on   (already needed to load the driver at all).
Command, deliberately left to you:
    verifier /standard /driver emuusbaudio.sys
    shutdown /r /t 0
Then re-run: .\ksoak.ps1 -Cycles 50
"@
    exit 0
}

if ($VerifierOff) {
    verifier /reset
    Write-Host "verifier settings reset; reboot required to disarm."
    exit 0
}

$dev = Get-EmuDevnode
if (-not $dev) { throw "USB\VID_041E&PID_3F04 not present - plug in the 0404." }
$id = $dev.InstanceId
$driver0 = $dev.Driver
Write-Host "devnode: $id  (driver $driver0)"
if ($driver0 -notmatch 'openemu') {
    Write-Warning "the devnode is bound to '$driver0', not the openEMU kernel package - run kinstall.ps1 first, otherwise this soak tests someone else's driver."
}

$pool0 = Get-PoolNonpaged
$events0 = Get-EventBaseline
Write-Host ("start: nonpaged pool {0:N0} B, System events (4h) {1}" -f $pool0, $events0)

$slow = @()
for ($c = 1; $c -le $Cycles; $c++) {
    $t0 = Get-Date
    $out = pnputil /disable-device $id | Out-String
    if ($LASTEXITCODE -ne 0) { throw "cycle ${c}: /disable-device failed (exit $LASTEXITCODE): $($out.Trim())" }
    if (-not (Wait-Disabled $id)) { throw "cycle ${c}: device never reached 'disabled' - the stop/remove path is stuck (remove lock, or a hanging PnP dispatch)." }
    $out = pnputil /enable-device $id | Out-String
    if ($LASTEXITCODE -ne 0) { throw "cycle ${c}: /enable-device failed (exit $LASTEXITCODE): $($out.Trim())" }
    if (-not (Wait-Ok $id)) {
        $d = Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue
        throw "cycle ${c}: device did not come back (status '$($d.Status)', problem $($d.ConfigManagerErrorCode))."
    }
    $elapsed = ((Get-Date) - $t0).TotalSeconds
    if ($elapsed -gt 3) { $slow += , [pscustomobject]@{ Cycle = $c; Seconds = $elapsed } }
    if (-not $Quiet -and ($c % 10 -eq 0)) {
        $pool = Get-PoolNonpaged
        Write-Host ("  cycle {0,3}/{1}: {2:N1}s, nonpaged pool {3:N0} B (delta {4:N0} B)" `
            -f $c, $Cycles, $elapsed, $pool, ($pool - $pool0))
    }
}

# remove/re-add: the devnode goes away and PnP must rebuild the FDO, so the
# device extension is freshly allocated each time. A physical replug exercises
# the same path plus surprise-remove; if you can, repeat this loop by hand.
for ($r = 1; $r -le $RemoveCycles; $r++) {
    $out = pnputil /remove-device $id | Out-String
    if ($LASTEXITCODE -ne 0) { throw "remove cycle ${r}: /remove-device failed (exit $LASTEXITCODE): $($out.Trim())" }
    Start-Sleep -Milliseconds 500
    $out = pnputil /scan-devices | Out-String
    if (-not (Wait-Ok $id)) { throw "remove cycle ${r}: device did not re-appear after /scan-devices - replug manually. (/scan-devices: $($out.Trim()))" }
    if (-not $Quiet) { Write-Host "  remove/re-add cycle $r/$RemoveCycles ok" }
}

$pool1 = Get-PoolNonpaged
$events1 = Get-EventBaseline
Write-Host ("end:   nonpaged pool {0:N0} B (delta {1:N0} B over {2} cycles)" -f $pool1, ($pool1 - $pool0), $Cycles)
Write-Host ("System events added during soak: {0}" -f ($events1 - $events0))

if ($slow.Count) {
    Write-Warning ("slow cycles (>3s): " + (($slow | ForEach-Object { "#$($_.Cycle) $($_.Seconds.ToString('N1'))s" }) -join ', '))
} else {
    Write-Host "no slow cycles: all disable/enable pairs settled quickly."
}

$new = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1,2,3; StartTime = (Get-Date).AddMinutes(-10) } `
        -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'emuusbaudio|EmuUSBAudio|portcls' } | Select-Object -First 10
if ($new) {
    Write-Host "related error/warning events:"
    $new | ForEach-Object { Write-Host "  [$($_.LevelDisplayName)] $($_.TimeCreated): $($_.Message)" }
} else {
    Write-Host "no emuusbaudio/portcls error events in the System log."
}

$d = Get-PnpDevice -InstanceId $id
Write-Host "final devnode status: $($d.Status), driver $($d.Driver)"
if ($d.Driver -ne $driver0) {
    Write-Warning "the devnode rebound from '$driver0' to '$($d.Driver)' - not every cycle ran against our driver."
}

$growthKB = ($pool1 - $pool0) / 1KB
if ($growthKB -gt 64) {
    Write-Warning ("nonpaged pool grew {0:N0} KB across the soak - investigate before calling M1 done." -f $growthKB)
} else {
    Write-Host "PASS: $Cycles disable/enable + $RemoveCycles remove/re-add cycles, pool delta within noise."
}
