[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package)

$ErrorActionPreference = 'Stop'
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$temp = (Resolve-Path -LiteralPath $env:TEMP).Path
$root = Join-Path $temp ('OpenCap-update-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$destination = Join-Path $root 'installed'
$mockInstalledVersionFile = Join-Path $destination 'OpenCap.version'
$mockChangeInstalledVersion = $false
$mockReleaseArchive = Join-Path $root 'release.zip'
$version = '99.1.0'
$expectedApi = 'https://api.github.com/repos/CrookedToe/OpenCap/releases/latest'
$listApi = 'https://api.github.com/repos/CrookedToe/OpenCap/releases?per_page=100&page=1'
$expectedDownload = "https://github.com/CrookedToe/OpenCap/releases/download/v$version/OpenCap-Windows-x64.zip"

function Invoke-RestMethod($Uri, $Headers, $UserAgent, [switch]$UseBasicParsing, $TimeoutSec) {
    if ($Uri -ceq $expectedApi) { return $mockRelease }
    if ($Uri -ceq $listApi) { return ,$mockReleases }
    throw 'Unexpected update metadata endpoint.'
}

function Mock-Release([string]$Version, [bool]$Prerelease, [bool]$Draft = $false) {
    $tag = "v$Version"
    return @{
        tag_name = $tag; draft = $Draft; prerelease = $Prerelease
        assets = @(@{
            name = 'OpenCap-Windows-x64.zip'; state = 'uploaded'
            browser_download_url = "https://github.com/CrookedToe/OpenCap/releases/download/$tag/OpenCap-Windows-x64.zip"
            digest = "sha256:$digest"; size = (Get-Item -LiteralPath $mockReleaseArchive).Length
        })
    }
}

function Invoke-WebRequest($Uri, $OutFile, $UserAgent, [switch]$UseBasicParsing, $TimeoutSec) {
    if ($Uri -cne $expectedDownload) { throw 'Unexpected update download URL.' }
    Copy-Item -LiteralPath $mockReleaseArchive -Destination $OutFile
    if ($mockChangeInstalledVersion) {
        Set-Content -LiteralPath $mockInstalledVersionFile -Value '98.1.0' -Encoding ascii
    }
}

try {
    $existing = (Get-Content -LiteralPath (Join-Path $packagePath 'OpenCap.version') -Raw).Trim()
    $installationSource = Join-Path $root 'downloaded'
    Copy-Item -LiteralPath $packagePath -Destination $installationSource -Recurse
    $installer = Join-Path $installationSource 'Install-OpenCap.ps1'
    foreach ($file in 'Install-OpenCap.ps1', 'SteamVRSession.ps1') {
        Set-Content -LiteralPath (Join-Path $installationSource $file) -Stream Zone.Identifier -Value @('[ZoneTransfer]', 'ZoneId=3')
    }
    # The supported setup entry point runs the installer after the user approves it.
    Unblock-File -LiteralPath $installer
    & $installer -Action Install -Destination $destination -SkipShortcut | Out-Null
    $sessionScript = Join-Path $destination 'SteamVRSession.ps1'
    if (Get-Item -LiteralPath $sessionScript -Stream Zone.Identifier -ErrorAction SilentlyContinue) {
        throw 'The installed script cannot run under RemoteSigned after extracting a browser download.'
    }
    $newPackage = Join-Path $root 'release'
    Copy-Item -LiteralPath $packagePath -Destination $newPackage -Recurse
    Set-Content -LiteralPath (Join-Path $newPackage 'OpenCap.version') -Value $version -Encoding ascii
    Compress-Archive -Path (Join-Path $newPackage '*') -DestinationPath $mockReleaseArchive
    $digest = (Get-FileHash -LiteralPath $mockReleaseArchive -Algorithm SHA256).Hash
    $mockRelease = @{
        tag_name = "v$version"
        draft = $false
        prerelease = $false
        assets = @(@{
            name = 'OpenCap-Windows-x64.zip'
            state = 'uploaded'
            browser_download_url = $expectedDownload
            digest = 'sha256:' + ('0' * 64)
            size = (Get-Item -LiteralPath $mockReleaseArchive).Length
        })
    }

    $installedScript = Join-Path $destination 'Install-OpenCap.ps1'
    $checked = & $installedScript -Action CheckForUpdate
    if ($checked[0] -notlike "*$version*") { throw 'The updater missed the newer release.' }
    & $installedScript -Action CheckForUpdate -Quiet
    if ($LASTEXITCODE -ne 2) { throw 'The updater did not signal the newer release to the desktop.' }
    try {
        & $installedScript -Action Update -Destination $destination -SkipShortcut -Apply | Out-Null
        throw 'The updater accepted a bad download digest.'
    } catch {
        if ($_.Exception.Message -notlike '*SHA-256 check*') { throw }
    }
    if ((Get-Content -LiteralPath $mockInstalledVersionFile -Raw).Trim() -ne $existing) {
        throw 'A corrupted update changed the current package.'
    }
    $mockRelease.assets[0].digest = "sha256:$digest"
    $mockChangeInstalledVersion = $true
    try {
        & $installedScript -Action Update -Destination $destination -SkipShortcut -Apply | Out-Null
        throw 'The updater installed over a version that changed during download.'
    } catch {
        if ($_.Exception.Message -notlike '*installed version changed*') { throw }
    }
    if ((Get-Content -LiteralPath $mockInstalledVersionFile -Raw).Trim() -ne '98.1.0') {
        throw 'A stale update changed the current package.'
    }
    $mockChangeInstalledVersion = $false
    Set-Content -LiteralPath $mockInstalledVersionFile -Value $existing -Encoding ascii

    & $installedScript -Action Update -Destination $destination -SkipShortcut -Apply | Out-Null
    if ((Get-Content -LiteralPath $mockInstalledVersionFile -Raw).Trim() -ne $version) {
        throw 'The verified update did not install.'
    }
    $checked = & $installedScript -Action CheckForUpdate
    if ($checked -notlike '*up to date*') { throw 'The updater offered the installed version.' }
    & $installedScript -Action Rollback -Destination $destination -SkipShortcut | Out-Null
    if ((Get-Content -LiteralPath $mockInstalledVersionFile -Raw).Trim() -ne $existing) {
        throw 'Rollback did not restore the pre-update package.'
    }

    $mockRelease = Mock-Release '99.1.0' $false
    $mockReleases = @((Mock-Release '99.3.0' $false $true), (Mock-Release '99.2.0-beta.2' $true),
        (Mock-Release '99.2.0-beta.10' $true), $mockRelease)
    $stable = & $installedScript -Action CheckForUpdate
    $beta = & $installedScript -Action CheckForUpdate -IncludePrerelease
    if ($stable[0] -notlike '*99.1.0 is available*' -or $beta[0] -notlike '*99.2.0-beta.10 is available*') {
        throw 'The updater did not honor channel selection, numeric prerelease ordering, or draft exclusion.'
    }
    Set-Content -LiteralPath $mockInstalledVersionFile -Value '99.2.0-beta.10' -Encoding ascii
    $stable = & $installedScript -Action CheckForUpdate
    if ($stable -notlike '*up to date*') { throw 'Opting out installed an older stable version.' }
    $mockRelease = Mock-Release '99.2.0' $false
    $mockReleases += $mockRelease
    $candidate = & $installedScript -Action CheckForUpdate -IncludePrerelease
    if ($candidate[0] -notlike '*99.2.0 is available*') { throw 'Stable should supersede the same version prerelease.' }

    $expectedDownload = 'https://github.com/CrookedToe/OpenCap/releases/download/v99.2.0-beta.11/OpenCap-Windows-x64.zip'
    Set-Content -LiteralPath (Join-Path $newPackage 'OpenCap.version') -Value '99.2.0-beta.11' -Encoding ascii
    Compress-Archive -Path (Join-Path $newPackage '*') -DestinationPath $mockReleaseArchive -Force
    $digest = (Get-FileHash -LiteralPath $mockReleaseArchive -Algorithm SHA256).Hash
    $mockReleases = @((Mock-Release '99.2.0-beta.11' $true))
    & $installedScript -Action Update -Destination $destination -SkipShortcut -Apply -IncludePrerelease | Out-Null
    if ((Get-Content -LiteralPath $mockInstalledVersionFile -Raw).Trim() -ne '99.2.0-beta.11') {
        throw 'The verified prerelease package did not install.'
    }
} finally {
    $item = Get-Item -LiteralPath $root -Force
    if ((Split-Path -Parent $item.FullName) -eq $temp -and
        -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $item.FullName -Recurse -Force
    }
}
