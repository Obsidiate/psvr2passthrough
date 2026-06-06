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
