<#
.SYNOPSIS
  run-ps-tests.ps1 - user-mode tests for the PowerShell-side logic
  (driver/ksign-lib.ps1) and static guards over ksign.ps1 / kinstall.ps1.

.DESCRIPTION
  The certificate-selection rules from docs/v3_plan.md P0.2/P0.3 are tested on
  fabricated certificate objects, never against a real store: the tests create
  PSCustomObjects with Subject / Thumbprint / HasPrivateKey / NotAfter /
  StoreLocation, which is all ksign-lib.ps1 reads. The one real certificate
  object below is an embedded self-signed PUBLIC certificate (generated with
  openssl, no private key exists for it in this repo) used to prove the
  public-only export path. Static guards pin the ksign.ps1 / kinstall.ps1
  wiring that pure functions cannot see: the library dot-source, the explicit
  -Thumbprint propagation to both signing calls, and the absence of the old
  "first certificate found" fallback.

  Run directly or through utest.sh:
      powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-ps-tests.ps1
  Prints "RESULT: PASS" or "RESULT: FAIL" like the C++ test binaries and exits
  non-zero on failure.

  SPDX-License-Identifier: LGPL-2.1-or-later
#>

$ErrorActionPreference = 'Stop'

$script:LibPath = Join-Path $PSScriptRoot '..\ksign-lib.ps1'
. $script:LibPath

# --- tiny harness ------------------------------------------------------------
$script:Pass = 0
$script:Fail = 0

function Start-Group([string]$Name) {
    Write-Host "-- $Name"
}

function Pass([string]$Name) {
    $script:Pass++
    Write-Host "   ok: $Name"
}

function Fail([string]$Name, [string]$Why) {
    $script:Fail++
    Write-Host "FAIL: $Name" -ForegroundColor Red
    if ($Why) { Write-Host "      $Why" -ForegroundColor Red }
}

function Assert-True([string]$Name, [bool]$Condition, [string]$Why = '') {
    if ($Condition) { Pass $Name } else { Fail $Name $Why }
}

function Assert-Throws([string]$Name, [scriptblock]$Block, [string]$Pattern = '') {
    try {
        & $Block | Out-Null
        Fail $Name 'expected a throw, got none'
    } catch {
        if ($Pattern -and ($_.Exception.Message -notmatch $Pattern)) {
            Fail $Name "message did not match /$Pattern/: $($_.Exception.Message)"
        } else {
            Pass $Name
        }
    }
}

# --- fixtures ----------------------------------------------------------------
$SUBJ = 'CN=openEMUdriver Test Signing'
$SUBJ_OTHER = 'CN=Some Other Publisher'

function Tp([string]$Prefix) { return $Prefix.PadRight(40, '0').ToUpperInvariant() }
$TP_E = Tp 'E01D'   # the CurrentUser\My cert of the 2026-11-08 machine
$TP_L = Tp '5820'   # the LocalMachine\My cert created the same day
$TP_X = Tp 'AAAA'   # never present

function New-MockCert {
    param(
        [Parameter(Mandatory)][string]$Thumbprint,
        [string]$Subject = $SUBJ,
        [string]$StoreLocation = 'CurrentUser',
        [bool]$HasPrivateKey = $true
    )
    $c = [pscustomobject]@{
        Subject       = $Subject
        Thumbprint    = $Thumbprint
        HasPrivateKey = $HasPrivateKey
        NotAfter      = (Get-Date '2031-01-01')
    }
    Add-Member -InputObject $c -NotePropertyName StoreLocation -NotePropertyValue $StoreLocation -Force
    return $c
}

# Machine state of 2026-11-08: two certificates, one subject, two stores.
function New-DuplicatePair {
    @(
        (New-MockCert -Thumbprint $TP_E -StoreLocation 'CurrentUser'),
        (New-MockCert -Thumbprint $TP_L -StoreLocation 'LocalMachine')
    )
}

# Self-signed public-only certificate (openssl fixture, no key anywhere).
$FIXTURE_DER_B64 = @'
MIIDMzCCAhugAwIBAgIUbqPK3FJQ5vG0nN95ID+x+fSI+gEwDQYJKoZIhvcNAQELBQAwKDEmMCQGA1UEAwwdcnVuLXBzLXRlc3RzIGZpeHR1cmUgKG5vIGtleSkwIBcNMjYxMTA4MDM0NDE4WhgPMjEyNjEwMTUwMzQ0MThaMCgxJjAkBgNVBAMMHXJ1bi1wcy10ZXN0cyBmaXh0dXJlIChubyBrZXkpMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsXknTL7jr5Xk8HZ4JGg8jtMwllk+sqvd27C/ISIQFCKjqlj5s8hI76UwdxrY5OghXZn2LavNUFwspFbjomq8RX3fPbfW41hJ9T9pMRy6I52LojwkwkkT92sBrWMxjzaa/neMRL4Wx2J/vz6uIOQd7LW2lUos/ErgWcoXMK8AgqxMFZ+N67Ll0+1UR5HWV4oWWZNx8qK/vkc1iJAYhLuauto/Vk37LtlqrKLS+ZYjppg5pWs2sqn0nmu8oq2QewIeOMCGVrDz1po9xpHtnmkLHjZQP2+McsYiNZDPMoU+rm4HRo3vFoYiF3QWFgs0l+vc8ZaWd9FOTcdJcF2I8OWyhwIDAQABo1MwUTAdBgNVHQ4EFgQUIfzNh+cJYqt/i+zWEpB0IjxLa0swHwYDVR0jBBgwFoAUIfzNh+cJYqt/i+zWEpB0IjxLa0swDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAnSPtbSPlZCwBYTpTRpv8vvd8+9qmmqYYi9h4/+NRkCJOsW74Vh/c4JGrgvR/yoFTY959dtXDTI4XvSJA9x3fO5x6KBBAr73YcodtnsFLZ67P1PxT2u6WS17DKFHMF4CrKfmlSFhVCgePk3nePZSzPc4UjqDUpgkSqlOX4RaE8awKtoeSLmyVI0y/+/AokCZqvwi1cuUbRDTU5vYxsa3rzcKee/Fbsz/We/ZhR2reDuvwBwEYJP0Xe2+Tzu44UKmpAyuHc5z5G9gB9LgpPxcJewSpHCNQ7hdmzSwb3LHbiZSqQRbSPde1Rau34N7kG13+GvPh3lFRwkmm6f7mvzogqA==
'@

# --- Get-NormalizedThumbprint -------------------------------------------------
Start-Group 'Get-NormalizedThumbprint'

$normalized = Get-NormalizedThumbprint ('  ' + $TP_E.ToLowerInvariant() + ' ')
Assert-True 'lower-case and spaces are normalized' ($normalized -ceq $TP_E) "got $normalized"

Assert-Throws 'too-short thumbprint is rejected' { Get-NormalizedThumbprint 'E01D' } '40-hex'
Assert-Throws 'non-hex thumbprint is rejected' { Get-NormalizedThumbprint ('Z' * 40) } '40-hex'

# --- Resolve-SigningCertificate: by thumbprint --------------------------------
Start-Group 'Resolve by thumbprint'

$pair = New-DuplicatePair
$hit = Resolve-SigningCertificate -Candidates $pair -Thumbprint $TP_E
Assert-True 'exact thumbprint wins over subject duplication' ($hit.Thumbprint -eq $TP_E) "got $($hit.Thumbprint)"

$hit = Resolve-SigningCertificate -Candidates $pair -Thumbprint ($TP_L.ToLowerInvariant())
Assert-True 'thumbprint matching is case-insensitive' ($hit.Thumbprint -eq $TP_L) "got $($hit.Thumbprint)"

Assert-Throws 'absent thumbprint stops (not present in stores)' { Resolve-SigningCertificate -Candidates $pair -Thumbprint $TP_X } 'not present'

try {
    Resolve-SigningCertificate -Candidates $pair -Thumbprint $TP_X | Out-Null
} catch {
    Assert-True 'absent-thumbprint error names the available certificates' ($_.Exception.Message -match [regex]::Escape($TP_E)) $_.Exception.Message
}

Assert-Throws 'malformed thumbprint argument stops' { Resolve-SigningCertificate -Candidates $pair -Thumbprint 'zz' } '40-hex'

# Same certificate imported into both My stores: prefer CurrentUser (no /sm).
$bothStores = @(
    (New-MockCert -Thumbprint $TP_E -StoreLocation 'CurrentUser'),
    (New-MockCert -Thumbprint $TP_E -StoreLocation 'LocalMachine')
)
$hit = Resolve-SigningCertificate -Candidates $bothStores -Thumbprint $TP_E
Assert-True 'cert in both stores: CurrentUser copy preferred' ($hit.StoreLocation -eq 'CurrentUser') "got $($hit.StoreLocation)"

# Same thumbprint, key only in LocalMachine: take that one rather than fail.
$keyOnlyLocal = @(
    (New-MockCert -Thumbprint $TP_E -StoreLocation 'CurrentUser' -HasPrivateKey $false),
    (New-MockCert -Thumbprint $TP_E -StoreLocation 'LocalMachine' -HasPrivateKey $true)
)
$hit = Resolve-SigningCertificate -Candidates $keyOnlyLocal -Thumbprint $TP_E
Assert-True 'key-less CurrentUser copy skipped, LocalMachine used' ($hit.StoreLocation -eq 'LocalMachine') "got $($hit.StoreLocation)"

$keylessOnly = @(New-MockCert -Thumbprint $TP_E -StoreLocation 'CurrentUser' -HasPrivateKey $false)
Assert-Throws 'thumbprint without private key cannot sign' { Resolve-SigningCertificate -Candidates $keylessOnly -Thumbprint $TP_E } 'no private key'

$hit = Resolve-SigningCertificate -Candidates $keylessOnly -Thumbprint $TP_E -PrivateKeyOptional
Assert-True 'key-less cert is fine for public-only resolution (-PrivateKeyOptional)' ($hit.Thumbprint -eq $TP_E) "got $($hit.Thumbprint)"

# --- Resolve-SigningCertificate: by subject -----------------------------------
Start-Group 'Resolve by subject'

Assert-Throws 'duplicate subjects stop, never first-found' { Resolve-SigningCertificate -Candidates (New-DuplicatePair) -Subject $SUBJ } 'ambiguous'

try {
    Resolve-SigningCertificate -Candidates (New-DuplicatePair) -Subject $SUBJ | Out-Null
} catch {
    $namesAll = ($_.Exception.Message -match [regex]::Escape($TP_E)) -and ($_.Exception.Message -match [regex]::Escape($TP_L))
    Assert-True 'ambiguity error names every duplicate' $namesAll $_.Exception.Message
}

Assert-Throws 'missing subject stops with the -CreateCert hint' { Resolve-SigningCertificate -Candidates (New-DuplicatePair) -Subject $SUBJ_OTHER } 'CreateCert'
Assert-Throws 'no thumbprint and no subject: no implicit pick' { Resolve-SigningCertificate -Candidates (New-DuplicatePair) } 'implicitly'

$single = @(New-MockCert -Thumbprint $TP_L -StoreLocation 'LocalMachine')
$hit = Resolve-SigningCertificate -Candidates $single -Subject $SUBJ
Assert-True 'unique subject resolves' ($hit.Thumbprint -eq $TP_L) "got $($hit.Thumbprint)"

$empty = @()
Assert-Throws 'empty candidate list stops by subject' { Resolve-SigningCertificate -Candidates $empty -Subject $SUBJ } 'no code-signing certificate'
Assert-Throws 'empty candidate list stops by thumbprint' { Resolve-SigningCertificate -Candidates $empty -Thumbprint $TP_E } 'not present'

# --- Get-CertStoreLocation ----------------------------------------------------
Start-Group 'Get-CertStoreLocation'

$plain = [pscustomobject]@{ Thumbprint = $TP_E }  # no StoreLocation, no PSParentPath
Assert-Throws 'undeterminable store location stops' { Get-CertStoreLocation $plain } 'store location'

$byParent = [pscustomobject]@{ Thumbprint = $TP_E }
Add-Member -InputObject $byParent -NotePropertyName PSParentPath -NotePropertyValue 'Microsoft.PowerShell.Security\Certificate::LocalMachine\My' -Force
Assert-True 'PSParentPath fallback: LocalMachine' ((Get-CertStoreLocation $byParent) -eq 'LocalMachine')

$badNote = New-MockCert -Thumbprint $TP_E -StoreLocation 'BogusStore'
Assert-Throws 'unknown StoreLocation note stops' { Get-CertStoreLocation $badNote } 'unknown StoreLocation'

# --- Get-SignToolStoreArgs ----------------------------------------------------
Start-Group 'Get-SignToolStoreArgs'

$userArgs = @(Get-SignToolStoreArgs (New-MockCert -Thumbprint $TP_E -StoreLocation 'CurrentUser'))
Assert-True 'CurrentUser cert: no /sm' ($userArgs.Count -eq 0) "got $($userArgs -join ' ')"

$machineArgs = @(Get-SignToolStoreArgs (New-MockCert -Thumbprint $TP_L -StoreLocation 'LocalMachine'))
Assert-True 'LocalMachine cert: /sm is added' ($machineArgs -contains '/sm') "got $($machineArgs -join ' ')"

# --- Assert-CertificateTrusted --------------------------------------------------
Start-Group 'Assert-CertificateTrusted'

$trustedOk = Assert-CertificateTrusted -Thumbprint $TP_E -TrustedThumbprints @($TP_L, $TP_E.ToLowerInvariant())
Assert-True 'trusted thumbprint passes' ($trustedOk -eq $true) "got $trustedOk"

Assert-Throws 'untrusted thumbprint stops with the -InstallCert fix' { Assert-CertificateTrusted -Thumbprint $TP_E -TrustedThumbprints @($TP_L) } 'InstallCert'
Assert-Throws 'empty trust list stops' { Assert-CertificateTrusted -Thumbprint $TP_E -TrustedThumbprints @() } 'not in LocalMachine'

# --- ConvertTo-PublicOnlyCertificate --------------------------------------------
Start-Group 'ConvertTo-PublicOnlyCertificate'

$fixture = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([Convert]::FromBase64String($FIXTURE_DER_B64))
$public = ConvertTo-PublicOnlyCertificate $fixture
Assert-True 'public copy keeps the thumbprint' ($public.Thumbprint -eq $fixture.Thumbprint) "$($public.Thumbprint) vs $($fixture.Thumbprint)"
Assert-True 'public copy is a real X509Certificate2 without a private key' (($public -is [System.Security.Cryptography.X509Certificates.X509Certificate2]) -and (-not $public.HasPrivateKey))
$roundTrip = [Convert]::ToBase64String($public.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Assert-True 'export is round-trip stable' ($roundTrip -eq $FIXTURE_DER_B64)

# --- static guards over ksign.ps1 / kinstall.ps1 ---------------------------------
Start-Group 'Static guards (scripts wire the library in)'

$ksign = Get-Content (Join-Path $PSScriptRoot '..\ksign.ps1') -Raw
$kinstall = Get-Content (Join-Path $PSScriptRoot '..\kinstall.ps1') -Raw
$lib = Get-Content $script:LibPath -Raw

Assert-True 'ksign.ps1 dot-sources ksign-lib.ps1' ($ksign -match 'Join-Path \$PSScriptRoot ''ksign-lib\.ps1''')
Assert-True 'kinstall.ps1 dot-sources ksign-lib.ps1' ($kinstall -match 'ksign-lib\.ps1')

$tpCalls = ([regex]::Matches($kinstall, [regex]::Escape('-Thumbprint $signCert.Thumbprint'))).Count
Assert-True "kinstall passes the resolved thumbprint to BOTH ksign calls (found $tpCalls)" ($tpCalls -eq 2) "found $tpCalls"

Assert-True 'the old first-found fallback is gone from ksign.ps1' (-not ($ksign -match 'using first available certificate'))
Assert-True 'ksign.ps1 checks trust before signing' ($ksign -match 'Assert-CertificateTrusted')

foreach ($fn in @('Get-SigningCandidates', 'Get-CertStoreLocation', 'Get-SignToolStoreArgs', 'Resolve-SigningCertificate', 'Assert-CertificateTrusted', 'ConvertTo-PublicOnlyCertificate', 'Get-NormalizedThumbprint')) {
    Assert-True "ksign-lib.ps1 defines $fn" ($lib -match "function $fn\b")
}

# --- verdict ---------------------------------------------------------------------
Write-Host ''
if ($script:Fail -ne 0) {
    Write-Host "RESULT: FAIL ($($script:Fail) failed, $($script:Pass) passed)"
    exit 1
}
Write-Host "RESULT: PASS ($script:Pass checks)"
exit 0
