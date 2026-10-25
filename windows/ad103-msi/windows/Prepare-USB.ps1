# SPDX-License-Identifier: GPL-2.0-only
# Prepare only the packaged, GPU-read-only diagnostic on an existing FAT32 USB.
[CmdletBinding()]
param([string]$DriveLetter)
$ErrorActionPreference = 'Stop'
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Run this preparer on Windows. It requires the Windows disk inventory commands.'
    }
    Import-Module (Join-Path $PSScriptRoot 'UsbPreparation.psm1') -Force
    Write-Host 'GPU Unlock: USB diagnostic preparation'
    Write-Host 'This copies the diagnostic to an existing FAT32 USB. It does not unlock the GPU.'
    Write-Host ''
    if (-not $DriveLetter) {
        $candidates = @(Get-GuDiagnosticUsbCandidates)
        if ($candidates.Count) {
            Write-Host 'USB volumes Windows can inspect:'
            $candidates | Select-Object DriveLetter, Label, FileSystem,
                @{Name='FreeGiB';Expression={[Math]::Round($_.FreeBytes / 1GB, 2)}} |
                Format-Table -AutoSize | Out-Host
        }
        Write-Host 'Use a spare USB with no existing EFI\BOOT\BOOTX64.EFI or GPU-UNLOCK-ADA folder.'
        Write-Host 'The USB must already be FAT32; this program never formats drives.'
        $DriveLetter = Read-Host 'Enter that USB drive letter (for example E), or press Enter to cancel'
        if ([string]::IsNullOrWhiteSpace($DriveLetter)) { Write-Host 'Cancelled. No files copied.'; exit 0 }
    }
    $result = New-GuDiagnosticUsb -PackageDirectory $PSScriptRoot -DriveLetter $DriveLetter
    Write-Host ''
    Write-Host ('Diagnostic USB prepared at ' + $result.Root) -ForegroundColor Green
    Write-Host ('Verified EFI SHA-256: ' + $result.Sha256)
    Write-Host 'Read START-HERE.txt before booting this USB. Preparing it does not restart your PC.'
    Write-Host 'Secure Boot, BitLocker, boot order, disks, and installed drivers were not reconfigured.'
} catch {
    Write-Host ''
    Write-Host ('USB preparation stopped: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Do not boot a partially prepared USB. Existing files were not intentionally overwritten.'
    Write-Host 'If Windows denied access to disk information, an Administrator PowerShell may be needed to inspect it.'
    exit 1
}
