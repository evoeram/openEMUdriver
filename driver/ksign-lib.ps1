<#
.SYNOPSIS
  Pure decision logic for ksign.ps1 and kinstall.ps1: pick the signing
  certificate, derive signtool store arguments, check trust, produce a
  public-only copy for the trusted stores.

.DESCRIPTION
  Every function takes its inputs as parameters (candidate lists, thumbprint
  sets) and touches no certificate store, so driver/tests/run-ps-tests.ps1 can
  feed fabricated certificate objects. Store access lives in
  Get-SigningCandidates and in the callers.

  Rules (docs/v3_plan.md P0.2, hardware lesson of 2026-11-08: the machine held
  two certificates with the same subject in different stores):
    - one explicitly identified thumbprint per signing run;
    - subject-based selection only when it matches exactly one certificate;
    - no fallback to "the first certificate found", ever;
    - a thumbprint that cannot sign (no private key) or will not load (not
      trusted) stops the run with an error that names the fix.

  SPDX-License-Identifier: LGPL-2.1-or-later
#>

# Thin store access: the only place that enumerates certificates. Annotates
# each candidate with the store it came from, so the rest of the logic and the
# tests never have to parse store paths.
function Get-SigningCandidates {
    $out = @()
    foreach ($loc in 'CurrentUser', 'LocalMachine') {
        $certs = @(Get-ChildItem "Cert:\$loc\My" -CodeSigningCert -ErrorAction SilentlyContinue)
        foreach ($c in $certs) {
            $c | Add-Member -NotePropertyName StoreLocation -NotePropertyValue $loc -Force
            $out += $c
        }
    }
    return , $out
}

function Get-NormalizedThumbprint {
    param([string]$Thumbprint)
    $t = ($Thumbprint -replace '\s', '').ToUpperInvariant()
    if ($t -notmatch '^[0-9A-F]{40}$') {
        throw "thumbprint '$Thumbprint' is not a 40-hex-character SHA-1 thumbprint; check the value passed via -Thumbprint."
    }
    return $t
}

# Where does this certificate live? signtool searches CurrentUser\My by default
# and needs /sm for LocalMachine\My, so a cert of unknown origin cannot be
# passed to signtool safely: fail instead of guessing.
function Get-CertStoreLocation {
    param([Parameter(Mandatory)]$Cert)
    $prop = $Cert.PSObject.Properties['StoreLocation']
    if ($prop -and $prop.Value) {
        $v = [string]$prop.Value
        if ($v -eq 'CurrentUser' -or $v -eq 'LocalMachine') { return $v }
        throw "certificate $($Cert.Thumbprint): unknown StoreLocation '$v'."
    }
    # PSParentPath looks like 'Microsoft.PowerShell.Security\Certificate::LocalMachine\My'.
    $pp = [string]$Cert.PSObject.Properties['PSParentPath'].Value
    if ($pp -match 'LocalMachine\\My\s*$') { return 'LocalMachine' }
    if ($pp -match 'CurrentUser\\My\s*$') { return 'CurrentUser' }
    throw "cannot determine the store location of certificate $($Cert.Thumbprint); take candidates from Get-SigningCandidates or annotate them with a StoreLocation property."
}

# signtool arguments that point it at the right store for this certificate.
function Get-SignToolStoreArgs {
    param([Parameter(Mandatory)]$Cert)
    if ((Get-CertStoreLocation $Cert) -eq 'LocalMachine') { return @('/sm') }
    return @()
}

# The single entry point for "which certificate signs this run".
#   -Thumbprint given: exact match required (the same certificate may sit in
#       both My stores - the CurrentUser copy is preferred because signtool
#       finds it without /sm). Missing or key-less certificate -> error.
#   -Thumbprint absent: -Subject must match exactly one candidate. Zero is an
#       error (never create a cert implicitly), more than one is an error
#       naming every duplicate (this is how the 2026-11-08 CurrentUser vs
#       LocalMachine mismatch is prevented from repeating silently).
# Private key is required unless -PrivateKeyOptional (public-only install).
function Resolve-SigningCertificate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()]$Candidates,
        [string]$Thumbprint,
        [string]$Subject,
        [switch]$PrivateKeyOptional
    )
    $needKey = -not $PrivateKeyOptional

    if ($Thumbprint) {
        $want = Get-NormalizedThumbprint $Thumbprint
        $hits = @($Candidates | Where-Object { (Get-NormalizedThumbprint ([string]$_.Thumbprint)) -eq $want })
        if ($hits.Count -eq 0) {
            $avail = if (@($Candidates).Count -eq 0) { "  (no code-signing certificates found in CurrentUser\My or LocalMachine\My)" }
                     else { (@($Candidates) | ForEach-Object { "  $($_.Thumbprint)  [$((Get-CertStoreLocation $_))\My]  $($_.Subject)" }) -join "`n" }
            throw "certificate $want is not present in the searched stores. Available code-signing certificates:`n$avail"
        }
        $withKey = @($hits | Where-Object { $_.HasPrivateKey } |
                     Sort-Object { if ((Get-CertStoreLocation $_) -eq 'CurrentUser') { 0 } else { 1 } })
        if ($withKey.Count -gt 0) {
            if ($withKey.Count -gt 1) {
                Write-Verbose "certificate $want exists in more than one My store; using $((Get-CertStoreLocation $withKey[0]))\My."
            }
            return $withKey[0]
        }
        if ($needKey) {
            throw "certificate $want is present but has no private key in any searched store, so it cannot sign. Import the PFX into the My store that holds the key, or pass the thumbprint of a certificate that has one."
        }
        return $hits[0]
    }

    if (-not $Subject) {
        throw "no -Thumbprint and no -Subject: refusing to pick a signing certificate implicitly. Pass -Thumbprint (preferred) or a -Subject that matches exactly one certificate."
    }

    $bySubject = @($Candidates | Where-Object { $_.Subject -eq $Subject })
    if ($bySubject.Count -eq 0) {
        throw "no code-signing certificate with subject '$Subject' in CurrentUser\My or LocalMachine\My. Create one: ./ksign.ps1 -CreateCert -InstallCert (elevated), or pass -Thumbprint."
    }
    if ($bySubject.Count -gt 1) {
        $list = (@($bySubject) | ForEach-Object { "  $($_.Thumbprint)  [$((Get-CertStoreLocation $_))\My]  expires $($_.NotAfter)" }) -join "`n"
        throw "ambiguous certificate selection: $($bySubject.Count) certificates share subject '$Subject'. Sign with one of them explicitly via -Thumbprint:`n$list"
    }
    $cert = $bySubject[0]
    if ($needKey -and -not $cert.HasPrivateKey) {
        throw "certificate $($cert.Thumbprint) matches subject '$Subject' but has no private key in this store and cannot sign. Import the PFX or pass -Thumbprint of a certificate that has a key."
    }
    return $cert
}

# A driver signed with a certificate outside Root / TrustedPublisher will not
# load: stop before signing instead of producing a package that fails at
# install time. Read-only on the stores, so it works without elevation.
function Assert-CertificateTrusted {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Thumbprint,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$TrustedThumbprints,
        [string]$Machine = 'LocalMachine'
    )
    $want = Get-NormalizedThumbprint $Thumbprint
    $have = @($TrustedThumbprints | ForEach-Object { Get-NormalizedThumbprint ([string]$_) })
    if ($have -notcontains $want) {
        throw "certificate $want is not in ${Machine}\Root or ${Machine}\TrustedPublisher, so a driver signed with it will not load on this machine. Fix: ./ksign.ps1 -InstallCert -Thumbprint $want (elevated). If this machine only signs packages for another machine, pass -NoTrustCheck."
    }
    return $true
}

# Trusted stores must never receive the private key: export the bare public
# certificate and add that copy.
function ConvertTo-PublicOnlyCertificate {
    param([Parameter(Mandatory)]$Cert)
    $publicBytes = $Cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    return [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($publicBytes)
}
