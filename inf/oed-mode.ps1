# oed-mode.ps1 - switch the E-MU USB device between driver modes
#   .\oed-mode.ps1 asio    - openEMUdriver direct WinUSB (low-latency ASIO only)
#   .\oed-mode.ps1 wasapi  - inbox usbaudio.sys (system WASAPI/MME, no openEMU ASIO)
# Run from an elevated PowerShell. SPDX-License-Identifier: LGPL-2.1-or-later
param([Parameter(Mandatory=$true)][ValidateSet('asio','wasapi')] [string]$Mode)

$ErrorActionPreference = 'Stop'
$infDir = $PSScriptRoot

function Get-OemPackages([string]$name) {
    # locale-independent AND substring-safe: the original INF name must appear
    # as a whole token on its own line (openemu.inf must not match
    # openemu-compat.inf blocks)
    $found = @()
    $out = pnputil /enum-drivers | Out-String
    $blocks = $out -split "`r?`n`r?`n"
    foreach ($b in $blocks) {
        $lines = $b -split "`r?`n"
        $exact = $lines | Where-Object { $_ -match ('(^|\s)' + [regex]::Escape($name) + '(\s|$)') }
        if ($exact) {
            if ($b -match '(oem\d+\.inf)') { $found += $Matches[1] }
        }
    }
    return $found
}

function Remove-Pkg([string]$name) {
    $oems = Get-OemPackages $name
    if ($oems.Count -gt 0) {
        foreach ($oem in $oems) {
            Write-Host "removing $name ($oem)"
            pnputil /delete-driver $oem /uninstall /force | Out-Null
        }
    } else {
        Write-Host "$name not present"
    }
}

function Install-Pkg([string]$inf) {
    Write-Host "installing $inf"
    pnputil /add-driver "$infDir\$inf" /install | Out-Null
}

function Restart-Device {
    $dev = (Get-PnpDevice | Where-Object {
        $_.InstanceId -like 'USB\VID_041E&PID_3F*' -and $_.InstanceId -notlike '*&MI_*'
    } | Select-Object -First 1).InstanceId
    if ($dev) {
        Write-Host "restarting $dev"
        pnputil /restart-device $dev | Out-Null
        Start-Sleep -Seconds 3
    } else {
        Write-Warning "E-MU device not found - replug the USB cable"
    }
}

# remove stale composite child nodes left from a previous binding
function Clean-StaleChildren {
    Get-PnpDevice | Where-Object {
        $_.InstanceId -like 'USB\VID_041E&PID_3F*&MI_*' -and $_.Status -eq 'Unknown'
    } | ForEach-Object {
        Write-Host "removing stale child $($_.InstanceId)"
        pnputil /remove-device "$($_.InstanceId)" | Out-Null
    }
}

if ($Mode -eq 'wasapi') {
    Remove-Pkg 'openemu.inf'
    Install-Pkg 'openemu-compat.inf'
} else {
    Remove-Pkg 'openemu-compat.inf'
    Install-Pkg 'openemu.inf'
}
Restart-Device
Clean-StaleChildren
Write-Host "done: $Mode mode"
