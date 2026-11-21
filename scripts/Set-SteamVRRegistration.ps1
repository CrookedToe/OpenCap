[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InstallPath,

    [string]$SteamVRPath,

    [switch]$Remove,

    [switch]$Dashboard
)

$ErrorActionPreference = 'Stop'
$install = (Resolve-Path -LiteralPath $InstallPath).Path
if (-not $SteamVRPath) {
    $vrPaths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (Test-Path -LiteralPath $vrPaths) {
        $SteamVRPath = (Get-Content -LiteralPath $vrPaths -Raw | ConvertFrom-Json).runtime |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ 'bin\win64\vrpathreg.exe') } |
            Select-Object -First 1
    }
    if (-not $SteamVRPath) {
        $SteamVRPath = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR'
    }
}
$vrpathreg = Join-Path $SteamVRPath 'bin\win64\vrpathreg.exe'
$driver = Join-Path $install 'steamvr\opencap'
$overlay = Join-Path $install 'OpenCapDashboard.exe'

if (-not (Test-Path -LiteralPath $vrpathreg -PathType Leaf)) { throw 'SteamVR vrpathreg.exe was not found.' }
if (-not (Test-Path -LiteralPath (Join-Path $driver 'driver.vrdrivermanifest') -PathType Leaf)) {
    throw 'InstallPath must point to a built OpenCap package.'
}
if ($Dashboard -and -not (Get-Process -Name vrserver -ErrorAction SilentlyContinue)) {
    throw 'Start SteamVR before changing dashboard registration.'
}

if ($Remove -and $Dashboard) {
    $process = Start-Process -FilePath $overlay -ArgumentList '--unregister' -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode) { throw "Dashboard removal failed with exit code $($process.ExitCode)." }
}

$operation = if ($Remove) { 'removedriver' } else { 'adddriver' }
& $vrpathreg $operation $driver
if ($LASTEXITCODE) { throw "vrpathreg $operation failed with exit code $LASTEXITCODE." }

if ($Dashboard -and -not $Remove) {
    $process = Start-Process -FilePath $overlay -ArgumentList '--register' -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode) { throw "Dashboard registration failed with exit code $($process.ExitCode)." }
}
