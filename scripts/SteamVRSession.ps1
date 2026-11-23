function Stop-VrSession {
    $processes = @(Get-Process -Name 'vrmonitor','vrserver','vrcompositor','vrdashboard' -ErrorAction SilentlyContinue)
    if (!$processes.Count) { return }
    foreach ($process in $processes) {
        if ($process.ProcessName -eq 'vrmonitor') { $process.CloseMainWindow() | Out-Null }
    }
    $until = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if (!(Get-Process -Name 'vrmonitor','vrserver','vrcompositor','vrdashboard' -ErrorAction SilentlyContinue)) { return }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    throw 'Exit SteamVR and run OpenCap setup again so SteamVR can reload the driver and dashboard settings.'
}

function Stop-GloveApps([bool]$IncludeVendor, [string]$PackagePath) {
    $desktops = @('OpenCap')
    $overlays = @('OpenCapDashboard')
    if ($IncludeVendor) {
        $desktops += @('UdcapDriver','ReUdcapDriver')
        $overlays += @('UDCAP_overlay','ReUDCAP_overlay')
    }
    foreach ($process in @(Get-Process -Name $desktops -ErrorAction SilentlyContinue)) {
        if ($PackagePath -and (!$process.Path -or (Split-Path -Parent $process.Path) -ne $PackagePath)) { continue }
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(3000)) { throw "Exit $($process.ProcessName) and run OpenCap setup again." }
    }
    foreach ($process in @(Get-Process -Name $overlays -ErrorAction SilentlyContinue)) {
        if ($PackagePath -and (!$process.Path -or (Split-Path -Parent $process.Path) -ne $PackagePath)) { continue }
        Stop-Process -InputObject $process
        if (!$process.WaitForExit(3000)) { throw "Exit $($process.ProcessName) and run OpenCap setup again." }
    }
}
