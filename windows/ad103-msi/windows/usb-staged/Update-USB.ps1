# SPDX-License-Identifier: GPL-2.0-only
[CmdletBinding()]
param([string]$DriveLetter)
$ErrorActionPreference = 'Stop'
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Run this updater on Windows.' }
    Import-Module (Join-Path $PSScriptRoot 'StagedUsb.psm1') -Force
    Write-Host 'GPU Unlock 0.6.4: test the candidate fix for unused USB children receiving new handles'
    Write-Host 'This updates only the exact released 0.6.3 USB and preserves assets, reports and all existing backups.'
    Write-Host 'Its existing firmware assets are verified in place. No Python or downloads are needed.'
    Write-Host 'At boot, U runs the actual payload, GPU register writes, DMA and one PCIe reset.'
    Write-Host 'The payload is unchanged. Only proved same-path unused USB leaf replacements are permitted; protected ownership stays checked.'
    Write-Host 'Windows handoff is still unverified; the test can stop, black-screen or hang. Read START-HERE.txt before booting.'
    if (-not $DriveLetter) {
        $candidates = @(Get-GuStagedUsbCandidates)
        if ($candidates.Count) { $candidates | Select-Object DriveLetter,Label,FileSystem,
            @{Name='FreeGiB';Expression={[Math]::Round($_.FreeBytes/1GB,2)}} | Format-Table -AutoSize | Out-Host }
        $DriveLetter = Read-Host 'Enter your existing 0.6.3 unlock-test USB letter (for example E), or Enter to cancel'
        if ([string]::IsNullOrWhiteSpace($DriveLetter)) { Write-Host 'Cancelled. No files changed.'; exit 0 }
    }
    $result = Update-GuStagedUsb -PackageDirectory $PSScriptRoot -DriveLetter $DriveLetter
    Write-Host ('USB ready: ' + $result.Root) -ForegroundColor Green
    Write-Host ('Verified EFI SHA-256: ' + $result.Sha256)
    Write-Host ('Previous EFI backup: ' + $result.BackupPath)
    Write-Host 'Preparation is complete. Read START-HERE.txt, including its boot and recovery instructions.'
    Write-Host 'Secure Boot, BitLocker, boot settings, disk partitions and installed drivers were not changed.'
} catch {
    Write-Host ('USB update stopped: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Keep the complete error. Do not boot a partially updated USB or delete backups/reports.'
    exit 1
}
