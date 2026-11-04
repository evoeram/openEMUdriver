<#
.SYNOPSIS
  ksign.ps1 [-File <path.sys>]... [-CertSubject <name>] [-Thumbprint <sha1>]
            [-NoTimestamp] [-CreateCert] [-Verify] [-InstallCert]

.DESCRIPTION
  Test-signs drivers built by driver/kbuild.sh. Signing is deliberately
  separate from the build: building must work without admin rights and without
  touching the certificate store.
  Default action: sign the given .sys files with an existing code-signing
  certificate (searched by -Thumbprint, else by -CertSubject in the
  CurrentUser/LocalMachine My store).
  -CreateCert  makes a self-signed test certificate (requires admin, writes to
               LocalMachine\My).
  -InstallCert copies that certificate into LocalMachine\Root and
               LocalMachine\TrustedPublisher so the test-signed driver loads
               (requires admin).
  -Verify      runs signtool verify /pa /v on the files.

  Loading the result still requires test signing mode on the target machine:
      bcdedit /set testsigning on     (then reboot)
  This script never changes the boot configuration; do it deliberately on a
  test machine, not on a working system. Release drivers need attestation
  signing through Partner Center instead of this path.

.EXAMPLE
  ./ksign.ps1 -CreateCert -InstallCert
  ./ksign.ps1 -File build\emuusbaudio.sys
  ./ksign.ps1 -File build\emuusbaudio.sys -Verify
#>
[CmdletBinding()]
param(
    [string[]]$File = @(),
    [string]$CertSubject = "CN=openEMUdriver Test Signing",
    [string]$Thumbprint,
    [switch]$NoTimestamp,
    [switch]$CreateCert,
    [switch]$InstallCert,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'

function Find-SignTool {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "$env:ProgramFiles\Windows Kits\10\bin"
    )
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem -Path $r -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match 'x64' } |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "signtool.exe not found. Install the Windows SDK (see docs/BUILD.md)."
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

$signtool = Find-SignTool
Write-Host "signtool: $signtool"

# --- certificate -----------------------------------------------------------
if ($CreateCert) {
    if (-not (Test-Admin)) { throw "-CreateCert writes to LocalMachine\My and needs an elevated shell." }
    $existing = Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert -ErrorAction SilentlyContinue |
                Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
    if ($existing) {
        Write-Host "certificate already present: $($existing.Thumbprint)"
    } else {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject $CertSubject -HashAlgorithm SHA256 -KeyExportPolicy Exportable `
            -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 `
            -NotAfter (Get-Date).AddYears(5) -CertStoreLocation Cert:\LocalMachine\My
        Write-Host "created test certificate: $($cert.Thumbprint)"
    }
}

if ($InstallCert) {
    if (-not (Test-Admin)) { throw "-InstallCert writes to LocalMachine stores and needs an elevated shell." }
    $src = Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert |
           Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
    if (-not $src) { throw "no certificate with subject '$CertSubject' in LocalMachine\My; run -CreateCert first." }
    foreach ($store in 'Root', 'TrustedPublisher') {
        $dst = New-Object Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
        $dst.Open('ReadWrite')
        if (-not ($dst.Certificates | Where-Object { $_.Thumbprint -eq $src.Thumbprint })) {
            $dst.Add($src)
            Write-Host "installed certificate into $store"
        } else {
            Write-Host "already in $store"
        }
        $dst.Close()
    }
}

if (-not $File) {
    $default = Join-Path $PSScriptRoot 'emuusbaudio\build\emuusbaudio.sys'
    if (Test-Path $default) { $File = @($default) }
    else { throw "no -File given and $default does not exist. Build first: driver/kbuild.sh" }
}

# --- pick certificate ------------------------------------------------------
$candidates = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue) +
              @(Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert -ErrorAction SilentlyContinue)
if ($Thumbprint) {
    $cert = $candidates | Where-Object { $_.Thumbprint -eq $Thumbprint.ToUpper() } | Select-Object -First 1
    if (-not $cert) { throw "certificate $Thumbprint not found in the My stores." }
} else {
    $cert = $candidates | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
    if (-not $cert) {
        $cert = $candidates | Select-Object -First 1
        if (-not $cert) { throw "no code-signing certificate found. Run: ./ksign.ps1 -CreateCert -InstallCert" }
        Write-Warning "using first available certificate: $($cert.Subject)"
    }
}
Write-Host "certificate: $($cert.Subject)  $($cert.Thumbprint)"

# --- sign ------------------------------------------------------------------
foreach ($f in $File) {
    if (-not (Test-Path $f)) { throw "file not found: $f" }
    $full = (Resolve-Path $f).Path
    $signArgs = @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint)
    if (-not $NoTimestamp) { $signArgs += @('/tr', 'http://timestamp.digicert.com', '/td', 'SHA256') }
    $signArgs += $full
    Write-Host "signing $full"
    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) {
        if (-not $NoTimestamp) {
            Write-Warning "timestamping failed; retrying without a timestamp counter-signature."
            & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $full
            if ($LASTEXITCODE -ne 0) { throw "signtool failed for $full" }
        } else { throw "signtool failed for $full" }
    }
    if ($Verify) {
        & $signtool verify /pa /v $full
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "verify /pa reports a problem (expected for a self-signed test certificate that is not in TrustedPublisher)."
        }
    }
}

Write-Host @"

Signed. To load on a test machine (not done by this script):
  bcdedit /set testsigning on      # then reboot
  pnputil /add-driver <oed.inf> /install
Release builds need Authenticode attestation signing instead.
"@
