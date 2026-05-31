# Removes the PSVR2 Passthrough Layer from both HKLM and HKCU implicit layer
# registries. Self-elevates to admin to handle the HKLM entry.

param([string]$ManifestPath = "")

# Discover manifest: alongside this script (release layout) or in the build tree (dev layout).
if (-not $ManifestPath) {
    if (Test-Path "$PSScriptRoot\PSVR2PassthroughLayer.json") {
        $ManifestPath = "$PSScriptRoot\PSVR2PassthroughLayer.json"
    } else {
        $ManifestPath = "$PSScriptRoot\..\build\src\layer\Release\PSVR2PassthroughLayer.json"
    }
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ManifestPath `"$ManifestPath`""
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

$ErrorActionPreference = "Stop"

# Resolve path if build dir exists; fall back to raw string so uninstall still
# works even after the build directory has been deleted.
try   { $manifest = (Resolve-Path $ManifestPath).Path }
catch { $manifest = $ManifestPath }

$removed = $false

$hklmKey = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
if (Test-Path $hklmKey) {
    $existing = Get-ItemProperty -Path $hklmKey -ErrorAction SilentlyContinue
    if ($existing.PSObject.Properties[$manifest]) {
        Remove-ItemProperty -Path $hklmKey -Name $manifest
        Write-Host "Unregistered from HKLM: $manifest"
        $removed = $true
    }
}

$hkcuKey = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
if (Test-Path $hkcuKey) {
    $existing = Get-ItemProperty -Path $hkcuKey -ErrorAction SilentlyContinue
    if ($existing.PSObject.Properties[$manifest]) {
        Remove-ItemProperty -Path $hkcuKey -Name $manifest
        Write-Host "Unregistered from HKCU: $manifest"
        $removed = $true
    }
}

if (-not $removed) {
    Write-Host "Layer was not registered (nothing to remove)."
}
