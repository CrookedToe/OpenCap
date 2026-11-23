[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package, [string]$SteamVRPath)

$ErrorActionPreference = 'Stop'
$setup = Join-Path (Resolve-Path -LiteralPath $Package).Path 'Setup-OpenCap.ps1'
$temp = (Resolve-Path -LiteralPath $env:TEMP).Path
$root = Join-Path $temp ('opencap-steamvr-test-' + [guid]::NewGuid().ToString('N'))
$oldLocal = $env:LOCALAPPDATA
$oldOverride = $env:VR_PATHREG_OVERRIDE

function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Read-Json($Path) { [IO.File]::ReadAllText($Path) | ConvertFrom-Json }
function Save-Json($Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10)) }
# Leave real desktop and SteamVR processes alone during simulated setup.
function Get-Process($Name, $ErrorAction) { }
$receiverId = 'USB\VID_1A86&PID_7523'
$receiverDriverFixture = [pscustomobject]@{ NeedsDriver = $false }
function Get-PnpDevice([switch]$PresentOnly) {
    [pscustomobject]@{ InstanceId = "$receiverId\test"; Status = $(if ($receiverDriverFixture.NeedsDriver) { 'Error' } else { 'OK' }) }
    [pscustomobject]@{ InstanceId = 'USB\VID_0000&PID_0000\test'; Status = 'Error' }
}
function Get-PnpDeviceProperty($InstanceId, $KeyName) {
    Require ($InstanceId -eq "$receiverId\test") 'Setup queried an unrelated device.'
    if ($KeyName -eq 'DEVPKEY_Device_ProblemCode') { return [pscustomobject]@{ Data = 28 } }
    if ($KeyName -eq 'DEVPKEY_Device_HardwareIds') { return [pscustomobject]@{ Data = @($receiverId) } }
    throw "Unexpected PnP property $KeyName"
}

function New-Object($ComObject) {
    if ($ComObject -eq 'Microsoft.Update.UpdateColl') {
        $collection = [pscustomobject]@{ Count = 0; SelectedId = '' }
        $collection | Add-Member -MemberType ScriptMethod -Name Add -Value {
            param($Update)
            $this.Count++
            $this.SelectedId = $Update.DriverHardwareID
        }
        return $collection
    }
    Require ($ComObject -eq 'Microsoft.Update.Session') 'Unexpected COM object during isolated setup.'
    $session = [pscustomobject]@{ ClientApplicationID = '' }
    $session | Add-Member -MemberType ScriptMethod -Name CreateUpdateSearcher -Value {
        $searcher = [pscustomobject]@{}
        $searcher | Add-Member -MemberType ScriptMethod -Name Search -Value {
            param($Criteria)
            Require ($Criteria -match "Type='Driver'") 'Setup searched for non-driver Windows updates.'
            return [pscustomobject]@{ Updates = @(
                [pscustomobject]@{ DriverHardwareID = 'USB\VID_1A86&PID_7524'; EulaAccepted = $true },
                [pscustomobject]@{ DriverHardwareID = 'USB\VID_0000&PID_0000'; EulaAccepted = $true },
                [pscustomobject]@{ DriverHardwareID = 'USB\VID_1A86&PID_7523'; EulaAccepted = $true }
            ) }
        }
        return $searcher
    }
    $session | Add-Member -MemberType ScriptMethod -Name CreateUpdateDownloader -Value {
        $downloader = [pscustomobject]@{ Updates = $null }
        $downloader | Add-Member -MemberType ScriptMethod -Name Download -Value {
            Require ($this.Updates.Count -eq 1 -and $this.Updates.SelectedId -eq 'USB\VID_1A86&PID_7523') 'Setup selected the wrong driver update.'
            return [pscustomobject]@{ ResultCode = 2 }
        }
        return $downloader
    }
    $session | Add-Member -MemberType ScriptMethod -Name CreateUpdateInstaller -Value {
        $installer = [pscustomobject]@{ Updates = $null; Fixture = $receiverDriverFixture }
        $installer | Add-Member -MemberType ScriptMethod -Name Install -Value {
            Require ($this.Updates.Count -eq 1 -and $this.Updates.SelectedId -eq 'USB\VID_1A86&PID_7523') 'Setup attempted to install the wrong driver.'
            $this.Fixture.NeedsDriver = $false
            return [pscustomobject]@{ ResultCode = 2; RebootRequired = $false }
        }
        return $installer
    }
    return $session
}

try {
    $env:LOCALAPPDATA = $root
    $env:VR_PATHREG_OVERRIDE = Join-Path $root 'openvrpaths.vrpath'
    $config = Join-Path $root 'config'
    $appConfigs = Join-Path $config 'vrappconfig'
    $vendor = Join-Path $root 'UDCAP Glove Driver'
    $otherDriver = Join-Path $root 'Other Driver'
    $oldDriver = Join-Path $root 'old\steamvr\opencap'
    $oldApp = Join-Path $root 'old\opencap.vrmanifest'
    $vendorApp = Join-Path $root 'vendor.vrmanifest'
    $sharedApp = Join-Path $root 'shared.vrmanifest'
    $addedManifest = Join-Path $root ('added-' + [char]0x00E9 + '.vrmanifest')
    $installed = Join-Path $root 'Programs\OpenCap'
    $installedDriver = Join-Path $installed 'steamvr\opencap'
    $installedApp = Join-Path $installed 'opencap.vrmanifest'
    $state = Join-Path $root 'OpenCap\steamvr-switch.json'
    $auto = Join-Path $appConfigs 'opencap.dashboard.vrappconfig'
    $vendorAuto = Join-Path $appConfigs 'reudcapdriver.driver.vrappconfig'
    $sharedAuto = Join-Path $appConfigs 'vendor.overlay.vrappconfig'
    $extraVendorAuto = Join-Path $appConfigs 'udcap.tools.vrappconfig'
    New-Item -ItemType Directory -Path $config,$appConfigs,$vendor,(Split-Path -Parent $oldApp) -Force | Out-Null
    $checkRuntime = [bool]$SteamVRPath
    if (!$checkRuntime) {
        $SteamVRPath = Join-Path $root 'SteamVR'
        New-Item -ItemType Directory -Path (Join-Path $SteamVRPath 'bin\win64') -Force | Out-Null
        New-Item -ItemType File -Path (Join-Path $SteamVRPath 'bin\win64\vrpathreg.exe') | Out-Null
    }
    $settings = Join-Path $config 'steamvr.vrsettings'
    $apps = Join-Path $config 'appconfig.json'
    Save-Json $env:VR_PATHREG_OVERRIDE @{ jsonid = 'vrpathreg'; version = 1; runtime = @($SteamVRPath); config = @($config);
        log = @($root); external_drivers = @($otherDriver,$vendor,$oldDriver) }
    Save-Json $settings @{ driver_UDCAP = @{ skeleton_only = $true }; steamvr = @{ showMirrorView = $true } }
    Save-Json $apps @{ manifest_paths = @($vendorApp,$sharedApp,$oldApp) }
    Save-Json (Join-Path $vendor 'driver.vrdrivermanifest') @{ name = 'udcap' }
    Save-Json $oldApp @{ applications = @(@{ app_key = 'opencap.dashboard' }) }
    Save-Json $vendorApp @{ applications = @(@{ app_key = 'reudcapdriver.driver' }) }
    Save-Json $sharedApp @{ applications = @(@{ app_key = 'unrelated.overlay' },
        @{ app_key = 'vendor.overlay'; binary_path_windows = 'bin\UDCAP_overlay.exe' }) }
    Save-Json $vendorAuto @{ autolaunch = $true; last_launch_time = '12' }
    Save-Json $extraVendorAuto @{ autolaunch = $true }

    $receiverDriverFixture.NeedsDriver = $true
    & $setup -Action InstallReceiverDriver
    Require (!$receiverDriverFixture.NeedsDriver) 'A missing receiver driver was not installed in the Windows Update fixture.'

    $originalPaths = Get-Content -LiteralPath $env:VR_PATHREG_OVERRIDE -Raw
    $originalSettings = Get-Content -LiteralPath $settings -Raw
    $busyAppConfig = [IO.File]::Open($apps, 'Open', 'Read', 'Read')
    $failed = $false
    try { & $setup -SkipShortcut } catch { $failed = $true }
    finally { $busyAppConfig.Dispose() }
    Require $failed 'The installation unexpectedly overwrote a locked SteamVR profile.'
    Require ((Get-Content -LiteralPath $env:VR_PATHREG_OVERRIDE -Raw) -ceq $originalPaths) 'Failed setup changed driver paths.'
    Require ((Get-Content -LiteralPath $settings -Raw) -ceq $originalSettings) 'Failed setup changed SteamVR settings.'
    Require (!(Test-Path $installed) -and !(Test-Path $state) -and (Read-Json $vendorAuto).autolaunch) 'Failed setup did not roll back.'

    & $setup -SkipShortcut
    & $setup -SkipShortcut
    Require (Test-Path (Join-Path $installed 'OpenCap.exe')) 'The desktop was not installed.'
    Require (Test-Path (Join-Path $installed 'OpenCapDashboard.exe')) 'The dashboard was not installed.'
    Require ((Read-Json $installedApp).applications[0].is_dashboard_overlay) 'Missing dashboard overlay declaration.'
    $expectedDrivers = "$otherDriver|$installedDriver"
    $actualDrivers = (Read-Json $env:VR_PATHREG_OVERRIDE).external_drivers -join '|'
    Require ($actualDrivers -eq $expectedDrivers) "Wrong installed driver list. Expected [$expectedDrivers]. Actual [$actualDrivers]."
    Require (((Read-Json $apps).manifest_paths -join '|') -eq "$sharedApp|$installedApp") 'Wrong installed dashboard list.'
    $active = Read-Json $settings
    Require ($active.driver_opencap.enable -and $active.driver_udcap.enable -eq $false -and $active.driver_udcap.skeleton_only) 'Incorrect installed driver settings.'
    Require ((Read-Json $auto).autolaunch -and !(Read-Json $vendorAuto).autolaunch -and
        !(Read-Json $sharedAuto).autolaunch -and !(Read-Json $extraVendorAuto).autolaunch) 'Incorrect overlay startup configuration.'
    if ($checkRuntime) {
        $paths = & (Join-Path $SteamVRPath 'bin\win64\vrpathreg.exe') show | Out-String
        Require ($LASTEXITCODE -eq 0 -and $paths.Contains($installedDriver) -and !$paths.Contains($vendor)) 'Valve vrpathreg did not see the installed driver.'
    }

    $active.steamvr.showMirrorView = $false
    Save-Json $settings $active
    Save-Json $apps @{ manifest_paths = @($sharedApp,$installedApp,$addedManifest) }
    & (Join-Path $installed 'Setup-OpenCap.ps1') -Action Remove -SkipShortcut
    Require (((Read-Json $env:VR_PATHREG_OVERRIDE).external_drivers -join '|') -eq "$otherDriver|$vendor") 'Wrong restored driver list.'
    Require (((Read-Json $apps).manifest_paths -join '|') -eq "$vendorApp|$sharedApp|$addedManifest") 'Wrong restored dashboard list.'
    $restored = Read-Json $settings
    Require (!$restored.driver_opencap -and !$restored.driver_udcap.PSObject.Properties['enable'] -and
        $restored.driver_udcap.skeleton_only -and !$restored.steamvr.showMirrorView) 'Driver settings were not restored without overwriting unrelated changes.'
    Require (!(Test-Path $auto) -and !(Test-Path $sharedAuto) -and (Read-Json $vendorAuto).autolaunch -and
        (Read-Json $vendorAuto).last_launch_time -eq '12' -and (Read-Json $extraVendorAuto).autolaunch) 'Overlay startup was not restored.'
    Require (!(Test-Path $installed) -and !(Test-Path "$installed.previous") -and !(Test-Path $state)) 'Removing OpenCap left installation files.'
    Require (Test-Path (Join-Path $vendor 'driver.vrdrivermanifest')) 'Vendor driver files were removed.'
    if ($checkRuntime) {
        $paths = & (Join-Path $SteamVRPath 'bin\win64\vrpathreg.exe') show | Out-String
        Require ($LASTEXITCODE -eq 0 -and $paths.Contains($vendor) -and !$paths.Contains($installedDriver)) 'Valve vrpathreg did not see the restored UDCAP driver.'
    }

    Save-Json $env:VR_PATHREG_OVERRIDE @{ jsonid = 'vrpathreg'; version = 1; runtime = @($SteamVRPath);
        config = @($config); log = @($root); external_drivers = @($otherDriver) }
    Save-Json $settings @{ steamvr = @{ showMirrorView = $false } }
    Save-Json $apps @{ manifest_paths = @($addedManifest) }
    Remove-Item -LiteralPath $vendorAuto,$extraVendorAuto
    & $setup -SkipShortcut
    Require (((Read-Json $env:VR_PATHREG_OVERRIDE).external_drivers -join '|') -eq "$otherDriver|$installedDriver") 'Setup depended on a UDCAP registration.'
    if (!(Test-Path -LiteralPath (Join-Path $SteamVRPath 'drivers\udcap\driver.vrdrivermanifest'))) {
        Require (!$((Read-Json $settings).PSObject.Properties['driver_udcap'])) 'Setup added a UDCAP section to a clean profile.'
    }
    & $setup -Action Remove -SkipShortcut
    Require (((Read-Json $env:VR_PATHREG_OVERRIDE).external_drivers -join '|') -eq $otherDriver) 'Removal registered a driver that was never installed.'
    Require (!$((Read-Json $settings).PSObject.Properties['driver_udcap'])) 'Removal left a UDCAP setting in a clean profile.'
    Write-Output 'SteamVR setup, clean-profile removal, failure rollback, and UDCAP restoration passed.'
} finally {
    $env:LOCALAPPDATA = $oldLocal
    $env:VR_PATHREG_OVERRIDE = $oldOverride
    if ((Split-Path -Parent $root) -eq $temp -and (Test-Path -LiteralPath $root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
