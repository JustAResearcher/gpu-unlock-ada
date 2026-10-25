# SPDX-License-Identifier: GPL-2.0-only
[CmdletBinding()]
param([string]$DriveLetter)
$ErrorActionPreference = 'Stop'
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Run this updater on Windows. It requires Windows disk inventory commands.'
    }
    Import-Module (Join-Path $PSScriptRoot 'RehearsalUsb.psm1') -Force
    Write-Host 'GPU Unlock: prepare the 0.5.2 firmware release/restoration rehearsal'
    Write-Host 'The boot test actively disconnects/reconnects firmware drivers after you press R.'
    Write-Host 'It can blank the display or hang. It does not run the unlock payload.'
    Write-Host '0.5.2 tests the observed display-initialization ordering fix. Physical behavior is unproven.'
    Write-Host 'This updater preserves reports and backs up the exact released 0.4.2 diagnostic or 0.5.0/0.5.1 rehearsal EFI.'
    Write-Host ''
    if (-not $DriveLetter) {
        $candidates = @(Get-GuRehearsalUsbCandidates)
        if ($candidates.Count) {
            Write-Host 'USB volumes Windows can inspect:'
            $candidates | Select-Object DriveLetter, Label, FileSystem,
                @{Name='FreeGiB';Expression={[Math]::Round($_.FreeBytes / 1GB, 2)}} |
                Format-Table -AutoSize | Out-Host
        }
        $DriveLetter = Read-Host 'Enter your existing 0.4.2 diagnostic or 0.5.0/0.5.1 rehearsal USB drive letter (for example E), or Enter to cancel'
        if ([string]::IsNullOrWhiteSpace($DriveLetter)) { Write-Host 'Cancelled. No files changed.'; exit 0 }
    }
    $result = Update-GuRehearsalUsb -PackageDirectory $PSScriptRoot -DriveLetter $DriveLetter
    Write-Host ''
    Write-Host ('Rehearsal USB updated at ' + $result.Root) -ForegroundColor Green
    Write-Host ('Verified EFI SHA-256: ' + $result.Sha256)
    Write-Host ('Previous EFI retained at: ' + $result.BackupPath)
    Write-Host 'Read START-HERE.txt before booting. Existing reports are unchanged.'
    Write-Host 'Secure Boot, BitLocker, boot order, disk partitions and installed drivers were not reconfigured.'
} catch {
    Write-Host ''
    Write-Host ('USB update stopped: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Do not boot a partially updated USB. No automatic deletion or recovery was attempted.'
    Write-Host 'Administrator PowerShell may be needed if Windows denied disk-inventory access.'
    exit 1
}
