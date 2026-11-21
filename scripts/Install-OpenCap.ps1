[CmdletBinding()]
param(
    [ValidateSet('Install', 'Rollback', 'Remove')]
    [string]$Action = 'Install',

    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\OpenCap'),

    [switch]$SkipShortcut
)

$ErrorActionPreference = 'Stop'
$destinationPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
$parent = Split-Path -Parent $destinationPath
$leaf = Split-Path -Leaf $destinationPath
if (-not $parent -or -not $leaf -or $destinationPath -eq [IO.Path]::GetPathRoot($destinationPath).TrimEnd('\', '/')) {
    throw 'Choose a dedicated installation directory.'
}
$sourcePath = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$previousPath = Join-Path $parent ($leaf + '.previous')
$stagePath = Join-Path $parent ($leaf + '.stage-' + [Guid]::NewGuid().ToString('N'))
$markerName = '.opencap-package'
$markerValue = 'OpenCap Windows package v1'
$required = @(
    'OpenCap.exe',
    'OpenCapDashboard.exe',
    'openvr_api.dll',
    'opencap.vrmanifest',
    'steamvr\opencap\driver.vrdrivermanifest',
    'steamvr\opencap\bin\win64\driver_opencap.dll',
    'Set-SteamVRRegistration.ps1',
    'Install-OpenCap.ps1',
    'LICENSE'
)

function Test-Package([string]$Path) {
    foreach ($file in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $file) -PathType Leaf)) { return $false }
    }
    return $true
}

function Assert-ManagedFolder([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $info = Get-Item -LiteralPath $resolvedPath -Force
    if (-not $info.PSIsContainer -or $resolvedPath -ne $Path -or
        (Split-Path -Parent $resolvedPath) -ne $parent -or
        ($info.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Choose a regular OpenCap package directory. Found $Path."
    }
    $marker = Join-Path $resolvedPath $markerName
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf) -or
        (Get-Content -LiteralPath $marker -Raw).Trim() -ne $markerValue) {
        throw "The directory is not marked as an OpenCap installation. Found $Path."
    }
    Assert-NoLinks $resolvedPath
}

function Assert-NoLinks([string]$Path) {
    $link = Get-ChildItem -LiteralPath $Path -Force -Recurse |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
        Select-Object -First 1
    if ($link) { throw "The package directory contains a link named $($link.Name)." }
}

function Update-Shortcut([bool]$Remove) {
    if ($SkipShortcut) { return }
    $programs = [Environment]::GetFolderPath('Programs')
    if (-not $programs) { return }
    $shortcutPath = Join-Path $programs 'OpenCap.lnk'
    $executable = Join-Path $destinationPath 'OpenCap.exe'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
        if ($shortcut.TargetPath -ne $executable) { return }
        if ($Remove) { Remove-Item -LiteralPath $shortcutPath -Force; return }
    }
    if ($Remove) { return }
    $shortcut.TargetPath = $executable
    $shortcut.WorkingDirectory = $destinationPath
    $shortcut.Description = 'OpenCap glove control'
    $shortcut.Save()
}

function Assert-ProgramsClosed {
    foreach ($process in (Get-Process -Name 'OpenCap','OpenCapDashboard' -ErrorAction SilentlyContinue)) {
        if ($process.Path -and (Split-Path -Parent $process.Path) -eq $destinationPath) {
            throw 'Close the installed OpenCap desktop and dashboard before replacing the package.'
        }
    }
}

function Remove-ManagedFolder([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Assert-ManagedFolder $Path
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

Assert-ManagedFolder $destinationPath
Assert-ManagedFolder $previousPath
Assert-ProgramsClosed

switch ($Action) {
    'Install' {
        if (-not (Test-Package $sourcePath)) { throw 'Run this script from a complete OpenCap Windows package.' }
        if ($sourcePath -eq $destinationPath -or $sourcePath -eq $previousPath) {
            throw 'Run an upgrade from a newly extracted OpenCap package.'
        }
        Assert-NoLinks $sourcePath
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        New-Item -ItemType Directory -Path $stagePath | Out-Null
        Set-Content -LiteralPath (Join-Path $stagePath $markerName) -Value $markerValue -Encoding ascii
        try {
            $items = Get-ChildItem -LiteralPath $sourcePath -Force | Where-Object Name -ne $markerName
            foreach ($item in $items) {
                Copy-Item -LiteralPath $item.FullName -Destination $stagePath -Recurse
            }
            if (-not (Test-Package $stagePath)) { throw 'The staged installation is incomplete.' }
            Assert-ManagedFolder $stagePath
            Remove-ManagedFolder $previousPath
            if (Test-Path -LiteralPath $destinationPath -PathType Container) {
                Assert-ManagedFolder $destinationPath
                Move-Item -LiteralPath $destinationPath -Destination $previousPath
            }
            try {
                Move-Item -LiteralPath $stagePath -Destination $destinationPath
            } catch {
                if (Test-Path -LiteralPath $previousPath -PathType Container) {
                    Assert-ManagedFolder $previousPath
                    Move-Item -LiteralPath $previousPath -Destination $destinationPath
                }
                throw
            }
        } finally {
            Remove-ManagedFolder $stagePath
        }
        Update-Shortcut $false
        Write-Output (Join-Path $destinationPath 'OpenCap.exe')
    }
    'Rollback' {
        if (-not (Test-Package $previousPath)) { throw 'No previous OpenCap package is available.' }
        if (-not (Test-Package $destinationPath)) { throw 'The current OpenCap package is incomplete.' }
        Move-Item -LiteralPath $destinationPath -Destination $stagePath
        try {
            Move-Item -LiteralPath $previousPath -Destination $destinationPath
        } catch {
            Assert-ManagedFolder $stagePath
            Move-Item -LiteralPath $stagePath -Destination $destinationPath
            throw
        }
        Assert-ManagedFolder $stagePath
        Move-Item -LiteralPath $stagePath -Destination $previousPath
        Update-Shortcut $false
        Write-Output (Join-Path $destinationPath 'OpenCap.exe')
    }
    'Remove' {
        Remove-ManagedFolder $destinationPath
        Remove-ManagedFolder $previousPath
        Update-Shortcut $true
        Write-Output 'OpenCap package removed. Settings are stored separately in Local AppData.'
    }
}
