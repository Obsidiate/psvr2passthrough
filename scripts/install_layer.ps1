# Registers the layer manifest under the system-wide implicit API layers key.
# SteamVR's embedded OpenXR runtime only reads HKLM (not HKCU) for implicit
# layers, so admin access is required. The script self-elevates if needed.

param(
    [string]$ManifestPath = "$PSScriptRoot\..\build\src\layer\Release\PSVR2PassthroughLayer.json"
)

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

# Remove any leftover HKCU registration (SteamVR ignores HKCU; stale entry
# would only appear in OpenXR Layers GUI and cause confusion).
$hkcuKey = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
if (Test-Path $hkcuKey) {
    $existing = Get-ItemProperty -Path $hkcuKey -ErrorAction SilentlyContinue
    if ($existing.PSObject.Properties[$manifest]) {
        Remove-ItemProperty -Path $hkcuKey -Name $manifest -ErrorAction SilentlyContinue
        Write-Host "Removed stale HKCU registration."
    }
}

$key = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }

# Enabled = 0, Disabled = 1 (yes, inverted — that's the Khronos convention).
New-ItemProperty -Path $key -Name $manifest -Value 0 -PropertyType DWord -Force | Out-Null
Write-Host "Registered PSVR2 Passthrough Layer (HKLM):"
Write-Host "  $manifest"
Write-Host ""
Write-Host "To uninstall: .\uninstall_layer.ps1"
