# Removes ALL PSVR2PassthroughLayer registrations from the OpenXR implicit
# layer registry keys in both HKLM and HKCU. Directory-agnostic — works
# regardless of where any version was installed from.

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

$ErrorActionPreference = "Stop"

$removed = $false

foreach ($hive in @("HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit",
                    "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit")) {
    if (-not (Test-Path $hive)) { continue }
    $props = Get-ItemProperty -Path $hive -ErrorAction SilentlyContinue
    if (-not $props) { continue }
    foreach ($name in $props.PSObject.Properties.Name) {
        if ([System.IO.Path]::GetFileName($name) -like "PSVR2PassthroughLayer*") {
            Remove-ItemProperty -Path $hive -Name $name -ErrorAction SilentlyContinue
            Write-Host "Unregistered: $name"
            $removed = $true
        }
    }
}

if (-not $removed) {
    Write-Host "No PSVR2 Passthrough Layer registrations found (nothing to remove)."
}

# ---------------------------------------------------------------------------
# OpenVR overlay: disable autolaunch and remove its SteamVR application manifest
# via PSVR2PassthroughOverlay.exe --unregister. Best-effort — it needs SteamVR
# running (Utility VR session); if SteamVR is down we tell the user how to finish.
$overlayExe = $null
$localExe = Get-ChildItem -Path $PSScriptRoot -Filter "PSVR2PassthroughOverlay.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($localExe) {
    $overlayExe = $localExe.FullName
} else {
    $buildExe = Get-ChildItem -Path "$PSScriptRoot\..\build\src\overlay\Release" -Filter "PSVR2PassthroughOverlay.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($buildExe) { $overlayExe = $buildExe.FullName }
}

if ($overlayExe) {
    Write-Host "Unregistering OpenVR overlay from SteamVR..."
    # GUI-subsystem exe: use Start-Process -Wait -PassThru to get the exit code.
    $ov = Start-Process -FilePath $overlayExe -ArgumentList "--unregister" -Wait -PassThru
    if ($ov.ExitCode -eq 0) {
        Write-Host "Unregistered OpenVR overlay (autolaunch disabled)."
    } else {
        Write-Warning "Could not unregister the OpenVR overlay (is SteamVR running?)."
        Write-Warning "Start SteamVR, then run:  `"$overlayExe`" --unregister"
    }
} else {
    Write-Host "PSVR2PassthroughOverlay.exe not found; skipped overlay unregistration."
}
