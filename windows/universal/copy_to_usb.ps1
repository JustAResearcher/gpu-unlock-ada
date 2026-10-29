param(
    [Parameter(Mandatory=$true)]
    [string]$DriveLetter
)

$DriveLetter = $DriveLetter.TrimEnd(':') + ":"
if (-not (Test-Path $DriveLetter)) {
    Write-Error "Drive $DriveLetter not found!"
    exit 1
}

$repo = $PSScriptRoot
Write-Host "Copying Universal GPU Unlock files to USB drive $DriveLetter..." -ForegroundColor Cyan

# 1. Dist EFI / BOOT
if (Test-Path "$repo\dist") {
    Copy-Item -Path "$repo\dist\*" -Destination "$DriveLetter\" -Recurse -Force
}
# 2. UNIVERSAL-ASSETS -> EFI\ASSETS
if (Test-Path "$repo\UNIVERSAL-ASSETS") {
    New-Item -ItemType Directory -Force -Path "$DriveLetter\EFI\ASSETS" | Out-Null
    Copy-Item -Path "$repo\UNIVERSAL-ASSETS\*" -Destination "$DriveLetter\EFI\ASSETS\" -Recurse -Force
}
# 3. EFI (VBIOS, etc)
if (Test-Path "$repo\EFI") {
    Copy-Item -Path "$repo\EFI\*" -Destination "$DriveLetter\EFI\" -Recurse -Force
}
# 4. Scripts and tools
if (Test-Path "$repo\dump_vbios.bat") { Copy-Item -Path "$repo\dump_vbios.bat" -Destination "$DriveLetter\" -Force }
if (Test-Path "$repo\dump_vbios.ps1") { Copy-Item -Path "$repo\dump_vbios.ps1" -Destination "$DriveLetter\" -Force }
if (Test-Path "$repo\nvflashk.exe") {
    Copy-Item -Path "$repo\nvflashk.exe" -Destination "$DriveLetter\" -Force
} elseif (Test-Path "$repo\..\nvflashk.exe") {
    Copy-Item -Path "$repo\..\nvflashk.exe" -Destination "$DriveLetter\" -Force
}
if (Test-Path "$repo\usb_release_staging\ИНСТРУКЦИЯ.txt") {
    Copy-Item -Path "$repo\usb_release_staging\ИНСТРУКЦИЯ.txt" -Destination "$DriveLetter\" -Force
}

Write-Host "Done! All files successfully deployed to $DriveLetter." -ForegroundColor Green
