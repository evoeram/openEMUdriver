<#
.SYNOPSIS
  kinstall.ps1 [-Build] [-Uninstall] [-Rollback] [-Sys <path>] [-Inf <path>]

.DESCRIPTION
  Installs the PortCls kernel driver (driver/emuusbaudio/Emu) on the live
  machine as an M1 PnP skeleton. Steps:
    1. stage emuusbaudio.sys next to inf/openemu-ks.inf in driver/package-ks
    2. Inf2Cat -> openemu-ks.cat
    3. test-sign the .sys and the .cat (delegates to ksign.ps1)
    4. drop the v1 WinUSB / compat packages so PnP rebinds the devnode to us
    5. pnputil /add-driver <inf> /install, then restart the devnode
  -Uninstall removes our package and reinstalls the v1 WinUSB driver.

  Needs: elevated shell, testsigning on (bcdedit /set testsigning on + reboot),
  and the test certificate from ksign.ps1 -CreateCert -InstallCert. This script
  never touches the boot configuration.

  v1 WinUSB and this kernel driver claim the same devnode: do not run the WinUSB
  ASIO host against the device while the kernel driver is bound (docs/V2_PLAN.md).

.EXAMPLE
  ./kinstall.ps1 -Build          # build, sign, install
  ./kinstall.ps1 -Uninstall      # back to v1 WinUSB
#>
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$Uninstall,
    [switch]$Rollback,
    [string]$Sys,
    [string]$Inf
)

$ErrorActionPreference = 'Stop'

$drvDir   = $PSScriptRoot
$repoRoot = Split-Path $drvDir -Parent
if (-not $Sys) { $Sys = Join-Path $drvDir 'emuusbaudio\build\emuusbaudio.sys' }
if (-not $Inf) { $Inf = Join-Path $repoRoot 'inf\openemu-ks.inf' }
$pkgDir  = Join-Path $drvDir 'package-ks'
$v1Inf   = Join-Path $repoRoot 'inf\openemu.inf'
$compatInf = Join-Path $repoRoot 'inf\openemu-compat.inf'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-KitTool([string]$name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin",
               "$env:ProgramFiles\Windows Kits\10\bin")
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem -Path $r -Recurse -Filter $name -ErrorAction SilentlyContinue |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "$name not found in the Windows SDK (see docs/BUILD.md)."
}

# kbuild.sh is a POSIX shell script and needs a real sh with Windows-path
# awareness - WSL's bash.exe (System32, WindowsApps) cannot see E:\ style paths.
function Find-ShBash {
    $candidates = @(
        "$env:ProgramFiles\Git\bin\bash.exe",
        "${env:ProgramFiles(x86)}\Git\bin\bash.exe",
        "$env:LOCALAPPDATA\hermes\git\bin\bash.exe"
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw "no Git-for-Windows bash.exe found; run driver/kbuild.sh from Git Bash and drop -Build."
}

# pnputil /enum-drivers is localized, so match the INF name as a whole token on
# its own line. 'openemu.inf' must not match the 'openemu-compat.inf' block.
# Hardware-run lesson: imports of the same INF name accumulate (one per
# DriverVer bump), and a devnode bound to the older import does NOT migrate to
# the newer one on /add-driver /install - the old binding has to be removed
# first, which is why Remove-OemPackage takes every match and why the install
# flow below removes 'openemu-ks.inf' before adding the fresh package.
function Get-OemPackages([string]$infName) {
    $out = pnputil /enum-drivers | Out-String
    $found = @()
    foreach ($block in ($out -split "`r?`n`r?`n")) {
        $hit = ($block -split "`r?`n") |
               Where-Object { $_ -match ('(^|\s)' + [regex]::Escape($infName) + '(\s|$)') }
        if ($hit -and $block -match '(oem\d+\.inf)') { $found += $Matches[1] }
    }
    return $found
}

function Get-OemPackage([string]$infName) {
    $all = Get-OemPackages $infName
    if ($all.Count -gt 0) { return $all[0] }
    return $null
}

function Remove-OemPackage([string]$infName) {
    $all = Get-OemPackages $infName
    if ($all.Count -gt 0) {
        foreach ($oem in $all) {
            Write-Host "removing $infName ($oem)"
            pnputil /delete-driver $oem /uninstall /force | Out-Null
        }
    } else {
        Write-Host "$infName not installed"
    }
}

function Get-EmuDevnode {
    Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'USB\VID_041E&PID_3F04*' } | Select-Object -First 1
}

function Show-Result {
    $dev = Get-EmuDevnode
    if (-not $dev) {
        Write-Warning "no USB\VID_041E&PID_3F04 devnode present - plug in the 0404 and run again."
        return
    }
    Write-Host "devnode : $($dev.InstanceId)"
    Write-Host "status  : $($dev.Status)  problem: $($dev.ConfigManagerErrorCode)"
    Write-Host "driver  : $($dev.Driver)"
    $svc = Get-Service EmuUSBAudio -ErrorAction SilentlyContinue
    if ($svc) {
        $state = (sc.exe query EmuUSBAudio | Out-String) -replace '\s+', ' '
        Write-Host "service : EmuUSBAudio $($state.Trim())"
    }
    $oem = Get-OemPackage 'openemu-ks.inf'
    if ($oem) { Write-Host "package : $oem" }
    # StartDevice logs on success; a failed bind shows up here instead.
    $recent = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1,2,3 } -MaxEvents 40 -ErrorAction SilentlyContinue |
              Where-Object { $_.Message -match 'emuusbaudio|EmuUSBAudio' } | Select-Object -First 5
    foreach ($e in $recent) { Write-Host "event   : [$($e.LevelDisplayName)] $($e.Message)" }
}

if ($Uninstall -or $Rollback) {
    if (-not (Test-Admin)) { throw "needs an elevated shell." }
    Remove-OemPackage 'openemu-ks.inf'
    Write-Host "restoring v1 WinUSB driver"
    if (Test-Path $v1Inf) { pnputil /add-driver $v1Inf /install | Out-Null }
    $dev = Get-EmuDevnode
    if ($dev) { pnputil /restart-device $dev.InstanceId | Out-Null }
    Show-Result
    exit 0
}

# --- build -----------------------------------------------------------------
if ($Build) {
    $bash = Find-ShBash
    Push-Location $drvDir
    try {
        & $bash ./kbuild.sh emuusbaudio/build/emuusbaudio.sys `
            emuusbaudio/Emu/emu_driver.cpp emuusbaudio/Emu/emu_pnp.cpp `
            emuusbaudio/Emu/emu_disp.cpp `
            emuusbaudio/Emu/emu_usb.cpp emuusbaudio/Emu/emu_desc.cpp `
            emuusbaudio/Emu/emu_ctrl.cpp emuusbaudio/Emu/emu_state.cpp `
            emuusbaudio/Emu/emu_ioctl.cpp
        if ($LASTEXITCODE -ne 0) { throw "kbuild.sh failed" }
    } finally { Pop-Location }
}
if (-not (Test-Path $Sys)) { throw "$Sys not found - build first (./kinstall.ps1 -Build)." }
if (-not (Test-Path $Inf)) { throw "$Inf not found." }
if (-not (Test-Admin)) {
    Write-Host "not elevated: staging, Inf2Cat and signing still run; the pnputil step at the end will fail."
}

# --- stage + catalog -------------------------------------------------------
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
Copy-Item $Sys (Join-Path $pkgDir 'emuusbaudio.sys') -Force
Copy-Item $Inf (Join-Path $pkgDir (Split-Path $Inf -Leaf)) -Force

$ksign = Join-Path $drvDir 'ksign.ps1'
$stagedSys = Join-Path $pkgDir 'emuusbaudio.sys'
$stagedInf = Join-Path $pkgDir (Split-Path $Inf -Leaf)

# sign the binary before the catalog exists, then the catalog itself: the
# conventional driver-package order, and it avoids any question about whether
# the appended signature changes the hashes Inf2Cat recorded.
& $ksign -File $stagedSys

$inf2cat = Find-KitTool 'Inf2Cat.exe'
Remove-Item (Join-Path $pkgDir '*.cat') -Force -ErrorAction SilentlyContinue
Write-Host "Inf2Cat $stagedInf"
# /os:10_X64 also covers Windows 11 (Inf2Cat has no 11_* token); /verbose makes
# this build of Inf2Cat bail with "Parameter format not correct".
& $inf2cat "/driver:$pkgDir" /os:10_X64
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed - check the INF against the SDK docs." }

$stagedCat = Join-Path $pkgDir (Get-Content $stagedInf |
    Select-String -Pattern '^\s*CatalogFile\w*\s*=\s*(\S+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1)
if (-not (Test-Path $stagedCat)) { throw "Inf2Cat produced no catalog at $stagedCat." }
& $ksign -File $stagedCat

# --- install ---------------------------------------------------------------
if (-not (Test-Admin)) { throw "pnputil /add-driver needs an elevated shell." }

# v1 (WinUSB) and compat (usbaudio.sys) claim the same devnode; drop them so
# PnP rebinds to the kernel driver. -Uninstall puts them back.
Remove-OemPackage 'openemu.inf'
Remove-OemPackage 'openemu-compat.inf'

$dev = Get-EmuDevnode
if (-not $dev) { Write-Warning "0404 not present: the package is installed but the device will bind on next plug-in." }

# The devnode must let go of the previous kernel-driver import before the new
# one can bind: /add-driver /install does not migrate an existing binding to a
# newer DriverVer of the same INF, so without this every rebuild after the
# first silently keeps running the old binary (hardware lesson 2026-11-05).
Remove-OemPackage 'openemu-ks.inf'

pnputil /add-driver $stagedInf /install

if ($dev) { pnputil /restart-device $dev.InstanceId | Out-Null }
Start-Sleep -Seconds 2
Show-Result

Write-Host @"

Next: the M1 acceptance run (50 disable/enable + remove/replug cycles under
Driver Verifier) - driver/ksoak.ps1. Verifier and reboot are not touched here.
"@
