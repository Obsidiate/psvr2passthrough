# Registers the layer manifest under the system-wide implicit API layers key.
# SteamVR's embedded OpenXR runtime only reads HKLM (not HKCU) for implicit
# layers, so admin access is required. The script self-elevates if needed.
#
# Before registering, removes any existing PSVR2PassthroughLayer entries from
# both HKLM and HKCU so stale registrations from previous installs are cleaned up.

param([string]$ManifestPath = "")

# Discover manifest: alongside this script (release layout) or in the build tree (dev layout).
# Matches versioned filenames e.g. PSVR2PassthroughLayer-051a.json as well as the plain name.
if (-not $ManifestPath) {
    $local = Get-ChildItem -Path $PSScriptRoot -Filter "PSVR2PassthroughLayer*.json" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($local) {
        $ManifestPath = $local.FullName
    } else {
        $build = Get-ChildItem -Path "$PSScriptRoot\..\build\src\layer\Release" -Filter "PSVR2PassthroughLayer*.json" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($build) { $ManifestPath = $build.FullName }
    }
    if (-not $ManifestPath) {
        Write-Error "No PSVR2PassthroughLayer*.json manifest found. Build the project first."
        exit 1
    }
}

# Self-elevate to admin if not already running elevated.
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ManifestPath `"$ManifestPath`""
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

$ErrorActionPreference = "Stop"

$manifest = (Resolve-Path $ManifestPath).Path
if (-not (Test-Path $manifest)) {
    Write-Error "Manifest not found at $manifest. Build the project first."
    exit 1
}

# Remove ALL existing PSVR2PassthroughLayer registrations from both HKLM and HKCU.
# This handles users upgrading from a previous install in a different folder.
foreach ($hive in @("HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit",
                    "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit")) {
    if (-not (Test-Path $hive)) { continue }
    $props = Get-ItemProperty -Path $hive -ErrorAction SilentlyContinue
    if (-not $props) { continue }
    foreach ($name in $props.PSObject.Properties.Name) {
        if ([System.IO.Path]::GetFileName($name) -like "PSVR2PassthroughLayer*") {
            Remove-ItemProperty -Path $hive -Name $name -ErrorAction SilentlyContinue
            Write-Host "Removed previous registration: $name"
        }
    }
}

$key = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }

# Enabled = 0, Disabled = 1 (yes, inverted — that's the Khronos convention).
New-ItemProperty -Path $key -Name $manifest -Value 0 -PropertyType DWord -Force | Out-Null
Write-Host "Registered PSVR2 Passthrough Layer (HKLM):"
Write-Host "  $manifest"
Write-Host ""

# ---------------------------------------------------------------------------
# OpenVR overlay (native-OpenVR games). Registers the overlay app with SteamVR
# and enables autolaunch via PSVR2PassthroughOverlay.exe --register. This does
# NOT need admin, but DOES need SteamVR running (it opens a Utility VR session),
# so it is best-effort: if it fails the layer install still succeeds and we tell
# the user how to register the overlay later.
$overlayExe = $null
$localExe = Get-ChildItem -Path $PSScriptRoot -Filter "PSVR2PassthroughOverlay.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($localExe) {
    $overlayExe = $localExe.FullName
} else {
    $buildExe = Get-ChildItem -Path "$PSScriptRoot\..\build\src\overlay\Release" -Filter "PSVR2PassthroughOverlay.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($buildExe) { $overlayExe = $buildExe.FullName }
}

if ($overlayExe) {
    Write-Host "Registering OpenVR overlay with SteamVR (autolaunch)..."
    # The overlay is a GUI-subsystem exe, so the call operator (&) does not wait
    # or set $LASTEXITCODE. Start-Process -Wait -PassThru does both.
    $ov = Start-Process -FilePath $overlayExe -ArgumentList "--register" -Wait -PassThru
    if ($ov.ExitCode -eq 0) {
        Write-Host "Registered OpenVR overlay (autostarts with SteamVR):"
        Write-Host "  $overlayExe"
    } else {
        Write-Warning "Could not register the OpenVR overlay (is SteamVR running?)."
        Write-Warning "Start SteamVR, then run:  `"$overlayExe`" --register"
    }
} else {
    Write-Warning "PSVR2PassthroughOverlay.exe not found; skipped overlay registration."
}

Write-Host ""
Write-Host "To uninstall: .\uninstall_layer.ps1"
