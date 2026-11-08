<#
.SYNOPSIS
  ksign.ps1 [-File <path.sys>]... [-CertSubject <name>] [-Thumbprint <sha1>]
            [-NoTimestamp] [-CreateCert] [-InstallCert] [-Verify] [-NoTrustCheck]

.DESCRIPTION
  Test-signs drivers built by driver/kbuild.sh. Signing is deliberately
  separate from the build: building must work without admin rights and without
  touching the certificate store.

  Exactly one certificate is used per run, and it is chosen deterministically:
  by -Thumbprint when given, otherwise by -CertSubject when that matches
  exactly one certificate in CurrentUser\My or LocalMachine\My. Any ambiguity
  (several certificates share the subject - the 2026-11-08 hardware lesson) or
  a missing key stops the run with an error naming every candidate; nothing is
  ever picked "first found". signtool is pointed at the right store
  automatically (/sm for LocalMachine\My certificates).

  -CreateCert  makes a self-signed test certificate (requires admin, writes to
               LocalMachine\My). Refuses to create a second certificate for a
               subject that already exists in either My store; prints the
               existing thumbprint(s) instead.
  -InstallCert copies the PUBLIC certificate only (the private key never
               leaves the My store) into LocalMachine\Root and
               LocalMachine\TrustedPublisher so the test-signed driver loads
               (requires admin).
  -Verify      runs signtool verify /pa /v on the files.
  -NoTrustCheck skips the "will this certificate actually load here" check for
               machines that only build packages for another machine.

  Loading the result still requires test signing mode on the target machine:
      bcdedit /set testsigning on     (then reboot)
  This script never changes the boot configuration; do it deliberately on a
  test machine, not on a working system. Release drivers need attestation
  signing through Partner Center instead of this path.

.EXAMPLE
  ./ksign.ps1 -CreateCert -InstallCert
  ./ksign.ps1 -File build\emuusbaudio.sys -Thumbprint E01D3BA3B04D19591DA0F161B56A22208F242CF0
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
    [switch]$Verify,
    [switch]$NoTrustCheck
)

$ErrorActionPreference = 'Stop'

# CLI-friendly stop: a failed run prints one actionable message and exits
# non-zero instead of a stack-trace style dump. Every throw below is written
# to name the fix (docs/v3_plan.md P0.2).
trap {
    Write-Host "ksign: ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

. (Join-Path $PSScriptRoot 'ksign-lib.ps1')

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

$candidates = Get-SigningCandidates

# --- certificate -----------------------------------------------------------
if ($CreateCert) {
    if ($Thumbprint) {
        # A thumbprint cannot be chosen at creation time (the key pair decides
        # it), so -CreateCert + -Thumbprint only ever makes sense as "verify
        # this certificate exists and use it".
        Get-NormalizedThumbprint $Thumbprint | Out-Null
        $existing = @($candidates | Where-Object {
            (Get-NormalizedThumbprint ([string]$_.Thumbprint)) -eq (Get-NormalizedThumbprint $Thumbprint) })
        if ($existing.Count -eq 0) {
            throw "-Thumbprint $Thumbprint is not present and -CreateCert cannot create a certificate with a chosen thumbprint. Drop -Thumbprint to create a new certificate and use the printed one."
        }
        Write-Host "certificate already present: $($existing[0].Thumbprint) [$((Get-CertStoreLocation $existing[0]))\My]"
    } else {
        $sameSubject = @($candidates | Where-Object { $_.Subject -eq $CertSubject })
        if ($sameSubject.Count -gt 0) {
            # Never a second certificate for a subject that already exists:
            # that is how the CurrentUser/LocalMachine mismatch of 2026-11-08
            # happened - -CreateCert only looked at LocalMachine\My.
            $list = (@($sameSubject) | ForEach-Object {
                "  $($_.Thumbprint)  [$((Get-CertStoreLocation $_))\My]  expires $($_.NotAfter)" }) -join "`n"
            Write-Host "certificate with subject '$CertSubject' already present, not creating another:`n$list"
            if ($sameSubject.Count -gt 1) {
                Write-Warning "more than one certificate shares this subject; sign with -Thumbprint <sha1> to choose one explicitly."
            }
        } else {
            if (-not (Test-Admin)) { throw "-CreateCert writes to LocalMachine\My and needs an elevated shell." }
            $cert = New-SelfSignedCertificate -Type CodeSigningCert `
                -Subject $CertSubject -HashAlgorithm SHA256 -KeyExportPolicy Exportable `
                -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 `
                -NotAfter (Get-Date).AddYears(5) -CertStoreLocation Cert:\LocalMachine\My
            $cert | Add-Member -NotePropertyName StoreLocation -NotePropertyValue 'LocalMachine' -Force
            $candidates += $cert
            Write-Host "created test certificate: $($cert.Thumbprint)"
        }
    }
}

if ($InstallCert) {
    if (-not (Test-Admin)) { throw "-InstallCert writes to LocalMachine stores and needs an elevated shell." }
    $src = Resolve-SigningCertificate -Candidates $candidates -Thumbprint $Thumbprint `
           -Subject $CertSubject -PrivateKeyOptional
    $public = ConvertTo-PublicOnlyCertificate $src
    foreach ($store in 'Root', 'TrustedPublisher') {
        $dst = New-Object Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
        $dst.Open('ReadWrite')
        try {
            if (-not ($dst.Certificates | Where-Object { $_.Thumbprint -eq $src.Thumbprint })) {
                # Add the public-only copy: the trusted stores never see the key.
                $dst.Add($public)
                Write-Host "installed public certificate into ${store}: $($src.Thumbprint)"
            } else {
                Write-Host "already in ${store}: $($src.Thumbprint)"
            }
        } finally { $dst.Close() }
    }
}

# --- files to sign ----------------------------------------------------------
$certOpOnly = [bool]($CreateCert -or $InstallCert)
if (-not $File) {
    $default = Join-Path $PSScriptRoot 'emuusbaudio\build\emuusbaudio.sys'
    if (Test-Path $default) { $File = @($default) }
    elseif ($certOpOnly) {
        Write-Host "no files to sign ($default absent); certificate operations only."
    } else {
        throw "no -File given and $default does not exist. Build first: driver/kbuild.sh"
    }
}

# --- pick the one signing certificate ---------------------------------------
$signCert = $null
if ($File) {
    $signCert = Resolve-SigningCertificate -Candidates $candidates -Thumbprint $Thumbprint -Subject $CertSubject
    Write-Host ("certificate: {0}  {1}  [{2}\My]" -f $signCert.Subject, $signCert.Thumbprint, (Get-CertStoreLocation $signCert))

    if (-not $NoTrustCheck) {
        $trusted = @(Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue) +
                   @(Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue)
        $null = Assert-CertificateTrusted -Thumbprint $signCert.Thumbprint `
            -TrustedThumbprints @($trusted | ForEach-Object { $_.Thumbprint })
    }
    $storeArgs = Get-SignToolStoreArgs $signCert
}

# --- sign -------------------------------------------------------------------
foreach ($f in @($File)) {
    if (-not (Test-Path $f)) { throw "file not found: $f" }
    $full = (Resolve-Path $f).Path
    $signArgs = @('sign', '/fd', 'SHA256', '/sha1', $signCert.Thumbprint) + $storeArgs
    if (-not $NoTimestamp) { $signArgs += @('/tr', 'http://timestamp.digicert.com', '/td', 'SHA256') }
    $signArgs += $full
    Write-Host "signing $full"
    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) {
        if (-not $NoTimestamp) {
            Write-Warning "timestamping failed; retrying without a timestamp counter-signature."
            & $signtool sign /fd SHA256 /sha1 $signCert.Thumbprint @storeArgs $full
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

if ($signCert) {
    Write-Host @"

Signed with thumbprint $($signCert.Thumbprint) - pass the same -Thumbprint to
every later signing run (kinstall.ps1 -Thumbprint does this for both the .sys
and the .cat). To load on a test machine (not done by this script):
  bcdedit /set testsigning on      # then reboot
  pnputil /add-driver <oed.inf> /install
Release builds need Authenticode attestation signing instead.
"@
}
