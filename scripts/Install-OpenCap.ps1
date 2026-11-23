[CmdletBinding()]
param(
    [ValidateSet('Install', 'Rollback', 'Remove', 'CheckForUpdate', 'Update')]
    [string]$Action = 'Install',

    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\OpenCap'),

    [switch]$SkipShortcut,

    [switch]$Quiet,

    [switch]$Apply,

    [switch]$IncludePrerelease,
    [switch]$Prompt,
    [string]$SteamVRStarter,
    [string]$ExpectedVersion
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

function ConvertTo-OpenCapVersion([string]$Text) {
    if ($Text -cnotmatch '^((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))(?:-([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$') { return }
    $core = $Matches[1]
    $suffix = $Matches[2]
    $parsed = $null
    if (![version]::TryParse($core, [ref]$parsed)) { return }
    $parts = if ($suffix) { $suffix -split '\.' }
    if (@($parts | Where-Object { $_ -cmatch '^0[0-9]+$' }).Count) { return }
    return [pscustomobject]@{ Text = $Text; Core = $parsed; Prerelease = @($parts | Where-Object { $_ }) }
}

function Compare-OpenCapVersion($First, $Second) {
    $normal = $First.Core.CompareTo($Second.Core)
    if ($normal) { return $normal }
    $firstParts = $First.Prerelease
    $secondParts = $Second.Prerelease
    if (!$firstParts.Count -or !$secondParts.Count) { return $secondParts.Count.CompareTo($firstParts.Count) }
    for ($i = 0; $i -lt [Math]::Min($firstParts.Count, $secondParts.Count); $i++) {
        $left = $firstParts[$i]
        $right = $secondParts[$i]
        $leftNumeric = $left -cmatch '^[0-9]+$'
        $rightNumeric = $right -cmatch '^[0-9]+$'
        if ($leftNumeric -ne $rightNumeric) { if ($leftNumeric) { return -1 }; return 1 }
        if ($leftNumeric -and $left.Length -ne $right.Length) { return $left.Length.CompareTo($right.Length) }
        $order = [String]::CompareOrdinal($left, $right)
        if ($order) { return $order }
    }
    return $firstParts.Count.CompareTo($secondParts.Count)
}

function Get-PackageVersion([string]$Path) {
    $file = Join-Path $Path 'OpenCap.version'
    $text = if (Test-Path -LiteralPath $file -PathType Leaf) { (Get-Content -LiteralPath $file -Raw).Trim() } else { '0.0.0' }
    $version = ConvertTo-OpenCapVersion $text
    if (!$version) { throw 'The package version is invalid.' }
    return $version
}

function Get-OpenCapRelease($Current) {
    $api = 'https://api.github.com/repos/CrookedToe/OpenCap/releases'
    $request = @{
        Headers = @{ Accept = 'application/vnd.github+json' }
        UserAgent = 'OpenCap Windows updater'
        UseBasicParsing = $true
        TimeoutSec = 15
    }
    $release = $null
    $releaseVersion = $Current
    for ($page = 1; ; $page++) {
        try {
            $uri = if ($IncludePrerelease) { "$($api)?per_page=100&page=$page" } else { "$api/latest" }
            $response = Invoke-RestMethod @request -Uri $uri
        } catch {
            if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 404 -and !$IncludePrerelease) { break }
            throw
        }
        $releases = @($response | Where-Object { $_ })
        foreach ($candidate in $releases) {
            if ($candidate.draft -or ($candidate.prerelease -and !$IncludePrerelease) -or
                $candidate.tag_name -cnotmatch '^v(.+)$') { continue }
            $version = ConvertTo-OpenCapVersion $Matches[1]
            if (!$version -or [bool]$version.Prerelease.Count -ne [bool]$candidate.prerelease -or
                (Compare-OpenCapVersion $version $releaseVersion) -le 0) { continue }
            $release = $candidate
            $releaseVersion = $version
        }
        if (!$IncludePrerelease -or $releases.Count -lt 100) { break }
    }
    if (!$release) { return }

    $name = 'OpenCap-Windows-x64.zip'
    $download = "https://github.com/CrookedToe/OpenCap/releases/download/$($release.tag_name)/$name"
    $packages = @($release.assets | Where-Object { $_.name -eq $name -and $_.state -eq 'uploaded' })
    if ($packages.Count -ne 1 -or $packages[0].browser_download_url -cne $download -or
        $packages[0].digest -notmatch '^sha256:[0-9a-fA-F]{64}$' -or
        $packages[0].size -lt 1 -or $packages[0].size -gt 100MB) {
        throw 'The release does not provide a verified Windows package.'
    }
    return [pscustomobject]@{
        Version = $releaseVersion.Text
        Tag = $release.tag_name
        Download = $download
        Hash = $packages[0].digest.Substring(7)
        Size = [long]$packages[0].size
    }
}

function Show-UpdateMessage([string]$Text, [bool]$Confirm) {
    Add-Type -AssemblyName System.Windows.Forms
    $form = New-Object System.Windows.Forms.Form
    try {
        $form.TopMost = $true
        $buttons = if ($Confirm) { 'YesNo' } else { 'OK' }
        $default = if ($Confirm) { 'Button2' } else { 'Button1' }
        return [Windows.Forms.MessageBox]::Show($form, $Text, 'OpenCap updates', $buttons, 'Information', $default)
    } finally {
        $form.Dispose()
    }
}


if ($Action -in @('CheckForUpdate', 'Update')) {
    $promptLock = $null
    $holdsPromptLock = $false
    $approved = $false
    try {
        if ($Prompt) {
            $promptLock = [Threading.Mutex]::new($false, 'Local\OpenCap.UpdatePrompt')
            try { $holdsPromptLock = $promptLock.WaitOne(0) }
            catch [Threading.AbandonedMutexException] { $holdsPromptLock = $true }
            if (!$holdsPromptLock) { return }
        }
        $installed = if ($Action -eq 'Update') { $destinationPath } else { $sourcePath }
        $current = Get-PackageVersion $installed
        $release = Get-OpenCapRelease $current
        if (!$release) {
            if (-not $Quiet) {
                $message = "OpenCap $($current.Text) is up to date on the selected update channel."
                if ($Prompt) { Show-UpdateMessage $message $false | Out-Null }
                else { Write-Output $message }
            }
            return
        }
        $available = "OpenCap $($release.Version) is available. Current version $($current.Text)."
        if ($Action -eq 'CheckForUpdate') {
            if ($Quiet) { exit 2 }
            $url = "https://github.com/CrookedToe/OpenCap/releases/tag/$($release.Tag)"
            if ($Prompt) {
                if ((Show-UpdateMessage "$available`n`nOpen the download page?" $true) -eq 'Yes') {
                    Start-Process -FilePath $url -WindowStyle Hidden
                }
            } else {
                Write-Output $available
                Write-Output $url
            }
            return
        }

        if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) {
            throw 'Install OpenCap before running an in-place update.'
        }
        Assert-ManagedFolder $destinationPath
        Assert-ManagedFolder $previousPath
        if ($Prompt) {
            if ((Show-UpdateMessage "$available`n`nInstall now? OpenCap will close and SteamVR will restart. This may close the running VR game." $true) -ne 'Yes') { return }
        } elseif (-not $Apply) {
            Write-Host $available
            Write-Host 'Close OpenCap and exit SteamVR before applying the update.'
            if ((Read-Host 'Apply the update? [y/N]') -ine 'y') { return }
        }
        $approved = $true
        if (!$Prompt) { Assert-ProgramsClosed }
        $temp = (Resolve-Path -LiteralPath $env:TEMP).Path
        $work = Join-Path $temp ('OpenCap-update-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $work | Out-Null
        $resumeVR = $false
        try {
            $archive = Join-Path $work 'OpenCap-Windows-x64.zip'
            $download = @{
                Uri = $release.Download
                OutFile = $archive
                UserAgent = 'OpenCap Windows updater'
                UseBasicParsing = $true
                TimeoutSec = 60
            }
            Invoke-WebRequest @download
            if ((Get-Item -LiteralPath $archive).Length -ne $release.Size -or
                (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $release.Hash) {
                throw 'The download failed its size or SHA-256 check.'
            }
            $package = Join-Path $work 'package'
            Expand-Archive -LiteralPath $archive -DestinationPath $package
            Assert-NoLinks $package
            if (-not (Test-Package $package) -or (Get-PackageVersion $package).Text -cne $release.Version -or
                !(Test-Path -LiteralPath (Join-Path $package 'SteamVRSession.ps1') -PathType Leaf)) {
                throw 'The downloaded package does not match the release.'
            }
            if ((Get-PackageVersion $destinationPath).Text -cne $current.Text) {
                throw 'The installed version changed while downloading. Check for updates again.'
            }
            if ($Prompt) {
                . (Join-Path $PSScriptRoot 'SteamVRSession.ps1')
                if (Get-Process -Name 'vrmonitor','vrserver','vrcompositor','vrdashboard' -ErrorAction SilentlyContinue) {
                    if (!$SteamVRStarter) {
                        $monitor = Get-Process -Name 'vrmonitor' -ErrorAction SilentlyContinue | Select-Object -First 1
                        if ($monitor.Path) { $SteamVRStarter = Join-Path (Split-Path -Parent $monitor.Path) 'vrstartup.exe' }
                    }
                    if (!$SteamVRStarter -or !(Test-Path -LiteralPath $SteamVRStarter -PathType Leaf)) {
                        throw 'Exit SteamVR and check for OpenCap updates again.'
                    }
                    Stop-VrSession
                    $resumeVR = $true
                }
                Stop-GloveApps $false $destinationPath
            }
            $installer = Join-Path $package 'Install-OpenCap.ps1'
            & $installer -Action Install -Destination $destinationPath -SkipShortcut:$SkipShortcut -ExpectedVersion $current.Text
        } finally {
            try {
                $workItem = Get-Item -LiteralPath $work -Force
                if ((Split-Path -Parent $workItem.FullName) -eq $temp -and
                    -not ($workItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                    Remove-Item -LiteralPath $workItem.FullName -Recurse -Force
                }
            } finally {
                if ($resumeVR) { Start-Process -FilePath $SteamVRStarter -WindowStyle Hidden }
            }
        }
        if ($Prompt -and !$resumeVR) { Show-UpdateMessage "OpenCap $($release.Version) was installed." $false | Out-Null }
    } catch {
        if (!$Prompt) { throw }
        if (!$Quiet -or $approved) { Show-UpdateMessage $_.Exception.Message $false | Out-Null }
    } finally {
        if ($holdsPromptLock) { $promptLock.ReleaseMutex() }
        if ($promptLock) { $promptLock.Dispose() }
    }
    return
}

$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $pathBytes = [Text.Encoding]::UTF8.GetBytes($destinationPath.ToUpperInvariant())
    $lockId = [BitConverter]::ToString($hasher.ComputeHash($pathBytes)).Replace('-', '')
} finally {
    $hasher.Dispose()
}
$installLock = [Threading.Mutex]::new($false, "Global\OpenCap.Install.$lockId")
$locked = $false
try {
    try {
        $locked = $installLock.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $locked = $true
    }
    if (-not $locked) { throw 'Another OpenCap installer is changing this installation.' }

    Assert-ManagedFolder $destinationPath
    Assert-ManagedFolder $previousPath
    Assert-ProgramsClosed

    switch ($Action) {
        'Install' {
            if ($ExpectedVersion -and ((Get-PackageVersion $destinationPath).Text -cne $ExpectedVersion -or
                -not (Test-Path -LiteralPath $destinationPath -PathType Container))) {
                throw 'The installed version changed while downloading. Check for updates again.'
            }
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
} finally {
    if ($locked) { $installLock.ReleaseMutex() }
    $installLock.Dispose()
}
