# Universal Multi-GPU VBIOS Dumper for Windows
# Formats ROM filenames strictly by hardware ID: <vendor>_<device>_<subvendor>_<subdevice>.rom

$ErrorActionPreference = "Continue"
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   UNIVERSAL MULTI-GPU VBIOS DUMPER (STRICT HARDWARE ID FORMAT)      " -ForegroundColor White
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$VbiosDir = Join-Path $ScriptDir "EFI\VBIOS"
if (-not (Test-Path $VbiosDir)) {
    New-Item -ItemType Directory -Path $VbiosDir -Force | Out-Null
}

$Nvflash = Join-Path $ScriptDir "..\nvflashk.exe"
if (-not (Test-Path $Nvflash)) {
    $Nvflash = Join-Path $ScriptDir "nvflashk.exe"
}
if (-not (Test-Path $Nvflash)) {
    $Nvflash = "nvflashk.exe"
}

# Query installed NVIDIA Display Controllers
$gpus = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -match "VEN_10DE" }

if (-not $gpus) {
    Write-Host "[!] No NVIDIA GPUs detected via WMI / PNP!" -ForegroundColor Red
    Pause
    exit 1
}

Write-Host "Found $($gpus.Count) NVIDIA GPU(s):" -ForegroundColor Green
$processedIds = @{}
$index = 0

foreach ($gpu in $gpus) {
    $name = $gpu.Name
    $pnp = $gpu.PNPDeviceID
    Write-Host "`n[GPU $index] $name" -ForegroundColor Yellow
    Write-Host "      PNP ID: $pnp"

    if ($pnp -match "VEN_10DE&DEV_([0-9A-Fa-f]{4})&SUBSYS_([0-9A-Fa-f]{4})([0-9A-Fa-f]{4})") {
        $devId = $Matches[1].ToLower()
        # In SUBSYS_<DEV><VEN>, the first 4 hex digits are Subsystem Device ID, next 4 are Subsystem Vendor ID
        $subDevId = $Matches[2].ToLower()
        $subVenId = $Matches[3].ToLower()
        $romName = "10de_${devId}_${subVenId}_${subDevId}.rom"
        $targetPath = Join-Path $VbiosDir $romName

        Write-Host "      Strict ROM Target: $romName" -ForegroundColor Cyan

        if ($processedIds.ContainsKey($romName)) {
            Write-Host "      [INFO] Identical hardware ID already dumped by sibling card." -ForegroundColor Green
            Write-Host "             The same file covers both cards automatically in UEFI loader!" -ForegroundColor Green
        } else {
            $processedIds[$romName] = $true
            Write-Host "      Dumping VBIOS via nvflashk (index $index)..."
            
            # Run nvflashk to save VBIOS by GPU index
            if (Test-Path $Nvflash) {
                & $Nvflash -i$index --save "$targetPath"
                if ($LASTEXITCODE -eq 0 -and (Test-Path $targetPath)) {
                    $sizeKb = [math]::Round((Get-Item $targetPath).Length / 1KB, 1)
                    Write-Host "      [SUCCESS] Dumped to $targetPath ($sizeKb KB)" -ForegroundColor Green
                } else {
                    Write-Host "      [TRY 2] Retrying with -b flag..."
                    & $Nvflash -i$index -b "$targetPath"
                    if ($LASTEXITCODE -eq 0 -and (Test-Path $targetPath)) {
                        Write-Host "      [SUCCESS] Dumped to $targetPath" -ForegroundColor Green
                    } else {
                        Write-Host "      [MANUAL NOTICE] If nvflashk failed, save VBIOS from GPU-Z as:" -ForegroundColor Yellow
                        Write-Host "                      $targetPath" -ForegroundColor Yellow
                    }
                }
            } else {
                Write-Host "      [MANUAL] nvflashk not found. Save VBIOS from GPU-Z as:" -ForegroundColor Yellow
                Write-Host "               $targetPath" -ForegroundColor Yellow
            }
        }
    } else {
        Write-Host "      [!] Could not parse Hardware ID from PNP ID." -ForegroundColor Red
    }
    $index++
}

Write-Host "`n======================================================================" -ForegroundColor Cyan
Write-Host "All VBIOS dumps are located in: $VbiosDir" -ForegroundColor Green
Write-Host "Copy this 'EFI' folder directly to your FAT32 USB drive to use with BOOTX64.EFI!"
Write-Host "======================================================================" -ForegroundColor Cyan

