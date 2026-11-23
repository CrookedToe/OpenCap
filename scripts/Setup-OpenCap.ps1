[CmdletBinding()]
param(
    [ValidateSet('Install', 'Remove', 'InstallReceiverDriver')]
    [string]$Action = 'Install',

    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\OpenCap'),

    [switch]$SkipShortcut,

    [switch]$StartSteamVR
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SteamVRSession.ps1')
$install = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
$installedDriver = Join-Path $install 'steamvr\opencap'
$installedManifest = Join-Path $install 'opencap.vrmanifest'
$packageInstaller = Join-Path $PSScriptRoot 'Install-OpenCap.ps1'
$statePath = Join-Path $env:LOCALAPPDATA 'OpenCap\steamvr-switch.json'
$pathsFile = if ($env:VR_PATHREG_OVERRIDE) { $env:VR_PATHREG_OVERRIDE }
             else { Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath' }
$pathsFile = [IO.Path]::GetFullPath($pathsFile)
$receiverHardware = '^USB\\(?:VID_1A86&PID_(?:7523|7524|0001)|VID_1A85&PID_7524|VID_1209&PID_0001|VID_1915&PID_521F)(?:[&\\]|$)'

function Get-ReceiversMissingDriver {
    Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match $receiverHardware -and $_.Status -ne 'OK' } |
        Where-Object { (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data -eq 28 }
}

function Install-ReceiverDriver {
    $missing = @(Get-ReceiversMissingDriver)
    if (!$missing.Count) { return }
    $hardwareIds = @(foreach ($device in $missing) {
        (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds').Data
    })
    $session = New-Object -ComObject Microsoft.Update.Session
    $session.ClientApplicationID = 'OpenCap Receiver Setup'
    $available = $session.CreateUpdateSearcher().Search("IsInstalled=0 and IsHidden=0 and Type='Driver'")
    $selected = New-Object -ComObject Microsoft.Update.UpdateColl
    $selectedIds = @()
    foreach ($update in ($available.Updates | Sort-Object DriverVerDate -Descending)) {
        $hardwareId = $update.DriverHardwareID
        if ($hardwareId -notmatch $receiverHardware -or $hardwareId -notin $hardwareIds -or $hardwareId -in $selectedIds) { continue }
        if (!$update.EulaAccepted) { $update.AcceptEula() }
        $selected.Add($update) | Out-Null
        $selectedIds += $hardwareId
    }
    if (!$selected.Count) { throw 'Windows Update did not offer a driver for the connected UDCAP receiver.' }
    $downloader = $session.CreateUpdateDownloader()
    $downloader.Updates = $selected
    if ($downloader.Download().ResultCode -ne 2) { throw 'Windows could not download the UDCAP receiver driver.' }
    $installer = $session.CreateUpdateInstaller()
    $installer.Updates = $selected
    $result = $installer.Install()
    if ($result.ResultCode -ne 2) { throw 'Windows could not install the UDCAP receiver driver.' }
    if ($result.RebootRequired) { exit 3010 }
    if (@(Get-ReceiversMissingDriver).Count) { throw 'Windows still reports a missing UDCAP receiver driver.' }
}

if ($Action -eq 'InstallReceiverDriver') { Install-ReceiverDriver; return }

function Initialize-ReceiverDriver {
    if (!@(Get-ReceiversMissingDriver).Count) { return }
    Write-Output 'Installing the missing UDCAP USB receiver driver through Windows Update.'
    $powerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Action InstallReceiverDriver' -f $PSCommandPath
    $process = Start-Process -FilePath $powerShell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -eq 3010) { throw 'Restart Windows to finish installing the USB receiver driver, then run OpenCap setup again.' }
    if ($process.ExitCode) { throw 'Windows could not install the USB receiver driver. Check Windows Update for the receiver driver before running setup again.' }
}

function Read-Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [pscustomobject]@{} }
    $value = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if ($value -isnot [pscustomobject]) { throw "Expected a JSON object in $Path." }
    return $value
}

function Set-Property($Object, [string]$Name, $Value) {
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { $property.Value = $Value }
    else { Add-Member -InputObject $Object -NotePropertyName $Name -NotePropertyValue $Value }
}

function Write-Bytes([string]$Path, [byte[]]$Bytes) {
    $folder = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $temp = Join-Path $folder ('.opencap-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllBytes($temp, $Bytes)
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temp, $Path) }
    } finally {
        if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
    }
}

function Save-Profile($Changes) {
    $before = [ordered]@{}
    try {
        foreach ($path in $Changes.Keys) {
            $before[$path] = if ([IO.File]::Exists($path)) { ,([IO.File]::ReadAllBytes($path)) } else { $null }
            if ($null -eq $Changes[$path]) { [IO.File]::Delete($path); continue }
            $json = ($Changes[$path] | ConvertTo-Json -Depth 100) + [Environment]::NewLine
            Write-Bytes $path ([Text.Encoding]::UTF8.GetBytes($json))
        }
    } catch {
        $failure = $_
        $recoveryErrors = @()
        foreach ($path in $before.Keys) {
            try {
                if ($null -eq $before[$path]) { [IO.File]::Delete($path) }
                else {
                    $original = [byte[]]$before[$path]
                    if (-not [IO.File]::Exists($path) -or
                        [Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -cne [Convert]::ToBase64String($original)) {
                        Write-Bytes $path $original
                    }
                }
            } catch { $recoveryErrors += $_.Exception.Message }
        }
        if ($recoveryErrors) { throw "$($failure.Exception.Message) SteamVR configuration recovery also failed. $recoveryErrors" }
        throw $failure
    }
}

function Same-Path([string]$First, [string]$Second) {
    return $First.Replace('/', '\').TrimEnd('\') -ieq $Second.Replace('/', '\').TrimEnd('\')
}

function Get-DriverName([string]$Path) {
    $manifest = Join-Path $Path 'driver.vrdrivermanifest'
    if ($Path -match '(?i)[\\/]steamvr[\\/]opencap[\\/]*$') { return 'opencap' }
    if ($Path -match '(?i)[\\/]UdcapDriver[\\/]udcap[\\/]*$') { return 'udcap' }
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        try { return (Read-Json $manifest).name } catch { return }
    }
}

function Is-VendorKey([string]$Key) {
    return $Key -match '(?i)(^|[._-])(?:re)?udcap(?:driver)?($|[._-])'
}

function Get-AppManifests($Paths) {
    foreach ($path in $Paths) {
        $items = @()
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            try { $items = @((Read-Json $path).applications | Where-Object { $null -ne $_ }) }
            catch { }
        }
        $keys = @($items | ForEach-Object {
            if ((Is-VendorKey $_.app_key) -or
                $_.binary_path_windows -match '(?i)(^|[\\/])(?:re)?udcap(?:driver|_overlay)?\.exe$') { $_.app_key }
        })
        [pscustomobject]@{
            Path = $path
            VendorKeys = $keys
            VendorOnly = $items.Count -gt 0 -and $keys.Count -eq $items.Count
            OpenCap = (Same-Path $path $installedManifest) -or
                ($items.Count -eq 1 -and $items[0].app_key -eq 'opencap.dashboard') -or
                (!(Test-Path -LiteralPath $path) -and (Split-Path -Leaf $path) -eq 'opencap.vrmanifest')
        }
    }
}

function Restore-Paths($Current, $Saved) {
    $list = [Collections.Generic.List[string]]::new()
    foreach ($path in $Current) { $list.Add($path) }
    foreach ($entry in $Saved) {
        if (!($list | Where-Object { Same-Path $_ $entry.Path })) {
            $list.Insert([Math]::Min([int]$entry.Index, $list.Count), [string]$entry.Path)
        }
    }
    return $list.ToArray()
}

function Remember-Paths($Paths, [scriptblock]$Match) {
    for ($index = 0; $index -lt $Paths.Count; ++$index) {
        if (& $Match $index) { [pscustomobject]@{ Path = $Paths[$index]; Index = $index } }
    }
}

function Remember-Property($Object, [string]$Name) {
    if ($null -eq $Object) { return [pscustomobject]@{ Exists = $false; Value = $null } }
    $property = $Object.PSObject.Properties[$Name]
    return [pscustomobject]@{ Exists = $null -ne $property; Value = $property.Value }
}

function Restore-Property($Object, [string]$Name, $Saved) {
    if ($Saved.Exists) { Set-Property $Object $Name $Saved.Value }
    else { $Object.PSObject.Properties.Remove($Name) }
}

if (!(Test-Path -LiteralPath $packageInstaller -PathType Leaf)) { throw 'Run setup from a complete OpenCap package.' }
$mutex = [Threading.Mutex]::new($false, 'Local\OpenCap.SteamVR.Setup')
$locked = $false
try {
    try { $locked = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Another OpenCap SteamVR setup is running.' }
    $hasState = Test-Path -LiteralPath $statePath -PathType Leaf
    $state = if ($hasState) { Read-Json $statePath } else { $null }
    if ($Action -eq 'Remove' -and !$hasState) { throw 'This Windows account has no saved OpenCap SteamVR switch to restore.' }
    if ($hasState -and ($state.Version -ne 1 -or !(Same-Path $state.Install $install) -or
        !(Same-Path $state.PathsFile $pathsFile))) {
        throw 'Restore the existing OpenCap installation using its original Windows account and installation path.'
    }
    if (!(Test-Path -LiteralPath $pathsFile -PathType Leaf)) {
        throw 'SteamVR has no OpenVR profile for this Windows account. Open SteamVR once and run setup again.'
    }
    if ($Action -eq 'Install') { Initialize-ReceiverDriver }
    Stop-VrSession
    Stop-GloveApps ($Action -eq 'Install')
    $paths = Read-Json $pathsFile
    $runtime = $paths.runtime | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'bin\win64\vrpathreg.exe') -PathType Leaf) } |
        Select-Object -First 1
    if (!$runtime) { throw 'The OpenVR profile does not point to an installed SteamVR runtime.' }
    $config = [string](@($paths.config | Where-Object { $_ } | Select-Object -First 1)[0])
    if (!$config -or ![IO.Path]::IsPathRooted($config) -or !(Test-Path -LiteralPath $config -PathType Container)) {
        throw 'OpenVR does not point to an existing SteamVR configuration directory.'
    }
    if ($hasState -and !(Same-Path $state.Config $config)) { throw 'The SteamVR configuration directory has changed since setup.' }
    $settingsPath = Join-Path $config 'steamvr.vrsettings'
    $appsPath = Join-Path $config 'appconfig.json'
    $appConfigFolder = Join-Path $config 'vrappconfig'
    $openCapAutoPath = Join-Path $appConfigFolder 'opencap.dashboard.vrappconfig'
    $settings = Read-Json $settingsPath
    $apps = Read-Json $appsPath
    $driverPaths = @($paths.external_drivers | Where-Object { $_ })
    $manifestPaths = @($apps.manifest_paths | Where-Object { $_ })
    $drivers = @($driverPaths | ForEach-Object { [pscustomobject]@{ Path = $_; Name = (Get-DriverName $_) } })
    $manifests = @(Get-AppManifests $manifestPaths)
    $otherDrivers = @($drivers | Where-Object { $_.Name -notin @('udcap','opencap') } | ForEach-Object { $_.Path })
    $udcapSection = $settings.PSObject.Properties['driver_udcap']
    $changes = [ordered]@{}

    if ($Action -eq 'Install') {
        $vendorKeys = @($manifests | ForEach-Object { $_.VendorKeys })
        if (Test-Path -LiteralPath $appConfigFolder -PathType Container) {
            $vendorKeys += @(Get-ChildItem -LiteralPath $appConfigFolder -Filter '*.vrappconfig' -File |
                Where-Object { Is-VendorKey $_.BaseName } | ForEach-Object { $_.BaseName })
        }
        if (!$hasState) {
            $vendorLaunch = @($vendorKeys | Sort-Object -Unique | Where-Object { $_ -match '^[a-zA-Z0-9._-]+$' } | ForEach-Object {
                $path = Join-Path $appConfigFolder "$_.vrappconfig"
                [pscustomobject]@{ Key = $_; Value = (Remember-Property (Read-Json $path) 'autolaunch') }
            })
            $state = [pscustomobject]@{
                Version = 1
                Install = $install
                PathsFile = $pathsFile
                Config = $config
                VendorDrivers = @(Remember-Paths $driverPaths { param($i) $drivers[$i].Name -eq 'udcap' })
                VendorManifests = @(Remember-Paths $manifestPaths { param($i) $manifests[$i].VendorOnly })
                VendorLaunch = $vendorLaunch
                VendorSection = $null -ne $udcapSection
                VendorEnabled = Remember-Property $udcapSection.Value 'enable'
            }
        }
        $builtInUdcap = Join-Path $runtime 'drivers\udcap\driver.vrdrivermanifest'
        if ($udcapSection -or $state.VendorDrivers -or (Test-Path -LiteralPath $builtInUdcap -PathType Leaf)) {
            if (!$udcapSection) { Set-Property $settings 'driver_udcap' ([pscustomobject]@{}) }
            Set-Property $settings.driver_udcap 'enable' $false
        }
        if (!$settings.driver_opencap) { Set-Property $settings 'driver_opencap' ([pscustomobject]@{}) }
        Set-Property $settings.driver_opencap 'enable' $true
        Set-Property $paths 'external_drivers' @($otherDrivers + $installedDriver)
        Set-Property $apps 'manifest_paths' @(
            $manifests | Where-Object { !$_.OpenCap -and !$_.VendorOnly } | ForEach-Object { $_.Path }
            $installedManifest
        )
        $changes[$statePath] = $state
        foreach ($item in $state.VendorLaunch) {
            if ($item.Key -notmatch '^[a-zA-Z0-9._-]+$') { throw 'Invalid overlay key in saved SteamVR state.' }
            $path = Join-Path $appConfigFolder "$($item.Key).vrappconfig"
            $value = Read-Json $path
            Set-Property $value 'autolaunch' $false
            $changes[$path] = $value
        }
        $auto = Read-Json $openCapAutoPath
        Set-Property $auto 'autolaunch' $true
        $changes[$openCapAutoPath] = $auto
        $changes[$pathsFile] = $paths
        $changes[$settingsPath] = $settings
        $changes[$appsPath] = $apps

        $refreshPackage = !(Same-Path $PSScriptRoot $install)
        $previousPackage = Test-Path -LiteralPath (Join-Path $install '.opencap-package') -PathType Leaf
        if ($refreshPackage) {
            & $packageInstaller -Destination $install -SkipShortcut:$SkipShortcut | Out-Null
        } elseif (!$previousPackage) { throw 'Run install from the extracted OpenCap package.' }
        try { Save-Profile $changes }
        catch {
            if ($refreshPackage) {
                $undo = if ($previousPackage) { 'Rollback' } else { 'Remove' }
                try { & $packageInstaller -Action $undo -Destination $install -SkipShortcut:$SkipShortcut | Out-Null }
                catch { Write-Warning $_.Exception.Message }
            }
            throw
        }
        Write-Output 'OpenCap is installed. SteamVR will start the OpenCap dashboard.'
    } else {
        Set-Property $paths 'external_drivers' @(Restore-Paths $otherDrivers $state.VendorDrivers)
        $unrelated = @($manifests | Where-Object { !$_.OpenCap } | ForEach-Object { $_.Path })
        Set-Property $apps 'manifest_paths' @(Restore-Paths $unrelated $state.VendorManifests)
        if ($udcapSection) {
            Restore-Property $udcapSection.Value 'enable' $state.VendorEnabled
            if (!$state.VendorSection -and !@($udcapSection.Value.PSObject.Properties).Count) {
                $settings.PSObject.Properties.Remove('driver_udcap')
            }
        } elseif ($state.VendorEnabled.Exists) {
            Set-Property $settings 'driver_udcap' ([pscustomobject]@{ enable = $state.VendorEnabled.Value })
        }
        $settings.PSObject.Properties.Remove('driver_opencap')
        foreach ($item in $state.VendorLaunch) {
            if ($item.Key -notmatch '^[a-zA-Z0-9._-]+$') { throw 'Invalid overlay key in saved SteamVR state.' }
            $path = Join-Path $appConfigFolder "$($item.Key).vrappconfig"
            $value = Read-Json $path
            Restore-Property $value 'autolaunch' $item.Value
            $changes[$path] = if (@($value.PSObject.Properties).Count) { $value } else { $null }
        }
        $changes[$openCapAutoPath] = $null
        $changes[$pathsFile] = $paths
        $changes[$settingsPath] = $settings
        $changes[$appsPath] = $apps
        Save-Profile $changes
        & $packageInstaller -Action Remove -Destination $install -SkipShortcut:$SkipShortcut | Out-Null
        [IO.File]::Delete($statePath)
        Write-Output 'OpenCap was removed and the previous SteamVR configuration was restored.'
    }
    if ($StartSteamVR) { Start-Process -FilePath 'steam://rungameid/250820' -WindowStyle Hidden }
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
