# SPDX-License-Identifier: GPL-2.0-only
# Update only the exact previous diagnostic EFI, preserving every report.
[CmdletBinding()]
param([string]$DriveLetter)
$ErrorActionPreference = 'Stop'
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Run this updater on Windows. It requires the Windows disk inventory commands.'
    }
    Import-Module (Join-Path $PSScriptRoot 'UsbPreparation.psm1') -Force
    Write-Host 'GPU Unlock: update an existing diagnostic USB'
    Write-Host 'This preserves your GPU-UNLOCK-ADA reports and saves a backup of the previous diagnostic.'
    Write-Host 'Only the exact released 0.4 and 0.4.1 EFIs are accepted. No other bootloader will be replaced.'
    Write-Host ''
    if (-not $DriveLetter) {
        $candidates = @(Get-GuDiagnosticUsbCandidates)
        if ($candidates.Count) {
            Write-Host 'USB volumes Windows can inspect:'
            $candidates | Select-Object DriveLetter, Label, FileSystem,
                @{Name='FreeGiB';Expression={[Math]::Round($_.FreeBytes / 1GB, 2)}} |
                Format-Table -AutoSize | Out-Host
        }
        $DriveLetter = Read-Host 'Enter your existing diagnostic USB drive letter (for example E), or press Enter to cancel'
        if ([string]::IsNullOrWhiteSpace($DriveLetter)) { Write-Host 'Cancelled. No files changed.'; exit 0 }
    }
    $result = Update-GuDiagnosticUsb -PackageDirectory $PSScriptRoot -DriveLetter $DriveLetter
    Write-Host ''
    Write-Host ('Diagnostic USB updated at ' + $result.Root) -ForegroundColor Green
    Write-Host ('Verified EFI SHA-256: ' + $result.Sha256)
    Write-Host ('Previous EFI retained at: ' + $result.BackupPath)
    Write-Host ('Previous EFI SHA-256: ' + $result.PreviousSha256)
    Write-Host 'Your reports are unchanged. Read START-HERE.txt before booting the diagnostic again.'
    Write-Host 'Secure Boot, BitLocker, boot order, disks, and installed drivers were not reconfigured.'
} catch {
    Write-Host ''
    Write-Host ('USB update stopped: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Do not boot a partially updated USB. No automatic deletion or recovery was attempted.'
    Write-Host 'If Windows denied access to disk information, an Administrator PowerShell may be needed to inspect it.'
    exit 1
}
