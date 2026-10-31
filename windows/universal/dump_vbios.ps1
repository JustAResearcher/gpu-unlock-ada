# Universal Multi-GPU VBIOS Dumper for Windows
# Formats ROM filenames strictly by hardware ID: <vendor>_<device>_<subvendor>_<subdevice>.rom
# Validates dumped VBIOS headers dynamically to ensure 100% correct Device ID matching

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

function Get-VbiosDevId($filePath) {
    if (-not (Test-Path $filePath)) { return $null }
    try {
        $bytes = [System.IO.File]::ReadAllBytes($filePath)
        for ($pos = 0; $pos -lt [Math]::Min($bytes.Length - 0x20, 0x100000); $pos += 512) {
            if ($bytes[$pos] -eq 0x55 -and $bytes[$pos+1] -eq 0xAA) {
                $pcirRel = [BitConverter]::ToUInt16($bytes, $pos + 0x18)
                $pcir = $pos + $pcirRel
                if ($pcir + 8 -le $bytes.Length) {
                    $sig = [System.Text.Encoding]::ASCII.GetString($bytes, $pcir, 4)
                    if ($sig -eq 'PCIR') {
                        $ven = [BitConverter]::ToUInt16($bytes, $pcir + 4)
                        $dev = [BitConverter]::ToUInt16($bytes, $pcir + 6)
                        if ($ven -eq 0x10DE) {
                            return ("{0:x4}" -f $dev).ToLower()
                        }
                    }
                }
            }
        }
    } catch {}
    return $null
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

        Write-Host "      Strict ROM Target: $romName (Expected DevID=0x$devId)" -ForegroundColor Cyan

        if ($processedIds.ContainsKey($romName)) {
            Write-Host "      [INFO] Identical hardware ID already dumped by sibling card." -ForegroundColor Green
            Write-Host "             The same file covers both cards automatically in UEFI loader!" -ForegroundColor Green
        } else {
            $processedIds[$romName] = $true
            $dumpSuccess = $false

            # Check candidate nvflashk adapter indices
            $candidateIndices = @($index) + (0..([Math]::Max(7, $gpus.Count * 2)) | Where-Object { $_ -ne $index })

            if (Test-Path $Nvflash) {
                foreach ($candidateIdx in $candidateIndices) {
                    Write-Host "      Attempting nvflashk with --index=$candidateIdx ..."
                    
                    # Try --save
                    & $Nvflash "--index=$candidateIdx" --save "$targetPath" 2>$null | Out-Null
                    if (-not (Test-Path $targetPath)) {
                        # Retry with -b
                        & $Nvflash "--index=$candidateIdx" -b "$targetPath" 2>$null | Out-Null
                    }

                    if (Test-Path $targetPath) {
                        $foundDev = Get-VbiosDevId $targetPath
                        if ($foundDev -eq $devId) {
                            $sizeKb = [math]::Round((Get-Item $targetPath).Length / 1KB, 1)
                            Write-Host "      [SUCCESS] Verified VBIOS dump for DevID 0x$devId via nvflashk --index=$candidateIdx ($sizeKb KB)" -ForegroundColor Green
                            $dumpSuccess = $true
                            break
                        } else {
                            Write-Host "      [MISMATCH] nvflashk index $candidateIdx returned DevID 0x$foundDev (expected 0x$devId). Discarding..." -ForegroundColor Yellow
                            Remove-Item $targetPath -Force -ErrorAction SilentlyContinue
                        }
                    }
                }
            }

            if (-not $dumpSuccess) {
                Write-Host "      [MANUAL NOTICE] Could not auto-dump via nvflashk." -ForegroundColor Yellow
                Write-Host "                      Please save VBIOS from GPU-Z as:" -ForegroundColor Yellow
                Write-Host "                      $targetPath" -ForegroundColor Yellow
            }
        }
    } else {
        Write-Host "      [!] Could not parse Hardware ID from PNP ID." -ForegroundColor Red
    }
    $index++
}

# Auto-sync dumps to FAT32 USB drive if connected
$usbDrives = Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -or $_.FileSystemLabel -match 'UNLOCK' }
foreach ($usb in $usbDrives) {
    if ($usb.DriveLetter) {
        $usbVbiosDir = "$($usb.DriveLetter):\EFI\VBIOS"
        if (Test-Path "$($usb.DriveLetter):\EFI\BOOT\BOOTX64.EFI") {
            if (-not (Test-Path $usbVbiosDir)) {
                New-Item -ItemType Directory -Path $usbVbiosDir -Force | Out-Null
            }
            Get-ChildItem -Path $VbiosDir -Filter "*.rom" | ForEach-Object {
                Copy-Item -Path $_.FullName -Destination $usbVbiosDir -Force
                Write-Host "`n[USB SYNC] Copied $($_.Name) to $usbVbiosDir" -ForegroundColor Green
            }
        }
    }
}

Write-Host "`n======================================================================" -ForegroundColor Cyan
Write-Host "All VBIOS dumps are located in: $VbiosDir" -ForegroundColor Green
Write-Host "======================================================================" -ForegroundColor Cyan
