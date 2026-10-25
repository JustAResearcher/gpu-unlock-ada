# SPDX-License-Identifier: GPL-2.0-only
# Windows storage is mocked; file operations exercise only a temporary directory.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$module = Import-Module (Join-Path $PSScriptRoot '../UsbPreparation.psm1') -Force -PassThru
$script:Checks = 0
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:Checks++
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    Assert $threw $Message
}
function Hash([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('gu-usb-tests-' + [Guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $fixture 'package'
$imagePath = Join-Path $packageRoot 'USB/EFI/BOOT/BOOTX64.EFI'
$manifestPath = Join-Path $packageRoot 'build-result.json'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($imagePath)) | Out-Null
# Minimal PE fixture, deliberately not an executable EFI program.
[byte[]]$fixtureBytes = New-Object byte[] 512
$fixtureBytes[0] = 77; $fixtureBytes[1] = 90; $fixtureBytes[60] = 128
$fixtureBytes[128] = 80; $fixtureBytes[129] = 69
$fixtureBytes[132] = 100; $fixtureBytes[133] = 134
$fixtureBytes[152] = 11; $fixtureBytes[153] = 2; $fixtureBytes[220] = 10
$fixtureHash = Hash $fixtureBytes
function Reset-Package {
    [IO.File]::WriteAllBytes($imagePath, $fixtureBytes)
    $manifest = [ordered]@{
        mode='diagnostic'; hardware_tested=$false; board_profile=$null; helper_protocol=$null
        sha256=$fixtureHash; size=$fixtureBytes.Length
    }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
function Set-Manifest([string]$Name, $Value) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.$Name = $Value
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
function New-Destination([hashtable]$Overrides=@{}, [string]$Mode='') {
    $destination = Join-Path $fixture ([Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    $values = @{
        DriveLetter='E'; Root=$destination; Label='Test USB'; DiskNumber=2; DiskUniqueId='disk-test'
        DiskPath='usb-path'; PartitionNumber=1; PartitionOffset=1048576; VolumeUniqueId='volume-test'
        VolumeSize=128MB; BusType='USB'; FileSystem='FAT32'; DiskIsBoot=$false; DiskIsSystem=$false
        DiskIsReadOnly=$false; DiskIsOffline=$false; PartitionIsBoot=$false; PartitionIsSystem=$false
        FreeBytes=16MB; SystemDrive='C:'
    }
    foreach ($key in $Overrides.Keys) { $values[$key] = $Overrides[$key] }
    & $module {
        param($Snapshot, $Mode)
        $script:TestSnapshot = $Snapshot; $script:SnapshotCalls = 0; $script:SnapshotMode = $Mode
        function script:Get-GuUsbSnapshot {
            param([string]$DriveLetter)
            $script:SnapshotCalls++
            $value = $script:TestSnapshot.PSObject.Copy()
            if (($script:SnapshotMode -eq 'change-before-write' -and $script:SnapshotCalls -eq 2) -or
                ($script:SnapshotMode -eq 'change-before-commit' -and $script:SnapshotCalls -eq 3) -or
                ($script:SnapshotMode -eq 'change-after-backup' -and $script:SnapshotCalls -eq 4)) {
                $value.DiskUniqueId = 'a-different-disk'
            }
            if (($script:SnapshotMode -eq 'foreign-boot-before-commit' -and $script:SnapshotCalls -eq 3) -or
                ($script:SnapshotMode -eq 'foreign-boot-after-backup' -and $script:SnapshotCalls -eq 4)) {
                [IO.File]::WriteAllText((Join-Path $value.Root 'EFI/BOOT/BOOTX64.EFI'), 'foreign boot image')
            }
            if (($script:SnapshotMode -eq 'approved-boot-before-write' -and $script:SnapshotCalls -eq 2) -or
                ($script:SnapshotMode -eq 'approved-boot-before-commit' -and $script:SnapshotCalls -eq 3)) {
                [IO.File]::WriteAllBytes((Join-Path $value.Root 'EFI/BOOT/BOOTX64.EFI'), $script:AlternatePreviousBytes)
            }
            return $value
        }
    } ([pscustomobject]$values) $Mode
    return $destination
}
try {
    Reset-Package
    $destination = New-Destination
    [IO.File]::WriteAllText((Join-Path $destination 'keep.txt'), 'unrelated user file')
    $prepared = New-GuDiagnosticUsb $packageRoot 'e:'
    Assert ($prepared.Root -eq $destination -and $prepared.Sha256 -ceq $fixtureHash) 'Preparation did not return verified destination/hash.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $fixtureHash) 'The USB image differs from the package.'
    Assert (Test-Path -LiteralPath (Join-Path $destination 'GPU-UNLOCK-ADA') -PathType Container) 'Report directory was not created.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'keep.txt') -Raw) -ceq 'unrelated user file') 'An unrelated user file changed.'
    Assert (@(Get-ChildItem (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Successful staging left temporary payloads.'
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'An already prepared USB was overwritten.'

    foreach ($bad in @('', 'E:/folder', '\\server\share', 'E:*', '1')) {
        Assert-Throws { New-GuDiagnosticUsb $packageRoot $bad } ('Invalid drive accepted: ' + $bad)
    }
    foreach ($override in @(
        @{BusType='NVMe'}, @{FileSystem='NTFS'}, @{FileSystem='exFAT'}, @{DiskIsBoot=$true},
        @{DiskIsSystem=$true}, @{PartitionIsBoot=$true}, @{PartitionIsSystem=$true},
        @{DiskIsReadOnly=$true}, @{DiskIsOffline=$true}, @{DiskIsBoot='False'},
        @{SystemDrive='E:'}, @{FreeBytes=1000}, @{VolumeUniqueId=''}, @{DiskUniqueId='';DiskPath=''})) {
        $destination = New-Destination $override
        Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } ('Unsafe/incomplete volume accepted: ' + ($override.Keys -join ','))
        Assert (@(Get-ChildItem -LiteralPath $destination -Force).Count -eq 0) 'Rejected volume was modified.'
    }
    foreach ($field in @('mode','hardware_tested','board_profile','helper_protocol','sha256','size')) {
        Reset-Package
        $destination = New-Destination
        switch ($field) {
            'mode' { Set-Manifest $field 'experimental-unlock' }
            'hardware_tested' { Set-Manifest $field 'false' }
            'board_profile' { Set-Manifest $field ([pscustomobject]@{device='2705'}) }
            'helper_protocol' { Set-Manifest $field 'gu-session-v1' }
            'sha256' { Set-Manifest $field ('0' * 64) }
            'size' { Set-Manifest $field 513 }
        }
        Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } ('Invalid manifest field accepted: ' + $field)
        Assert (@(Get-ChildItem -LiteralPath $destination -Force).Count -eq 0) 'Bad package modified the USB.'
    }
    Reset-Package
    $destination = New-Destination
    [IO.File]::WriteAllText((Join-Path $packageRoot 'USB/extra.sys'), 'unexpected payload')
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Extra payload file was accepted.'
    Remove-Item -LiteralPath (Join-Path $packageRoot 'USB/extra.sys')
    $altered = $fixtureBytes.Clone(); $altered[0] = 0
    [IO.File]::WriteAllBytes($imagePath, $altered)
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Corrupt EFI with stale hash was accepted.'
    Set-Manifest 'sha256' (Hash $altered)
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Non-PE payload with a matching hash was accepted.'
    Reset-Package

    $destination = New-Destination
    [IO.Directory]::CreateDirectory((Join-Path $destination 'GPU-UNLOCK-ADA')) | Out-Null
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Existing report directory was accepted.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $destination 'EFI'))) 'Collision preflight modified the USB.'
    $destination = New-Destination
    [IO.Directory]::CreateDirectory((Join-Path $destination 'EFI/BOOT')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'), 'original bootloader')
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Existing bootloader was accepted.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Raw) -ceq 'original bootloader') 'Existing bootloader changed.'

    $destination = New-Destination @{} 'change-before-write'
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'USB hot-swap before mutation was accepted.'
    Assert (@(Get-ChildItem -LiteralPath $destination -Force).Count -eq 0) 'Changed disk was modified before copy.'
    $destination = New-Destination @{} 'change-before-commit'
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'USB hot-swap before final image creation was accepted.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'))) 'A bootable name was created after identity changed.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Partial staging was deleted without inspection.'
    $destination = New-Destination @{} 'foreign-boot-before-commit'
    Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Late bootloader collision was overwritten.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Raw) -ceq 'foreign boot image') 'Late foreign bootloader was altered.'

    # Symbolic-link creation on Windows can require developer mode/admin. Run these
    # checks on the Linux host, where they need no elevated privileges.
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        $destination = New-Destination
        $foreign = Join-Path $fixture 'foreign'; [IO.Directory]::CreateDirectory($foreign) | Out-Null
        New-Item -ItemType SymbolicLink -Path (Join-Path $destination 'EFI') -Target $foreign | Out-Null
        Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Destination ancestor link was accepted.'
        Assert (@(Get-ChildItem -LiteralPath $foreign -Force).Count -eq 0) 'Copy escaped through the destination link.'
        $destination = New-Destination
        [IO.Directory]::CreateDirectory((Join-Path $destination 'EFI/BOOT')) | Out-Null
        New-Item -ItemType SymbolicLink -Path (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Target (Join-Path $foreign 'missing.efi') | Out-Null
        Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Dangling destination link was accepted.'
        New-Item -ItemType SymbolicLink -Path (Join-Path $packageRoot 'USB/linked') -Target $foreign | Out-Null
        Assert-Throws { New-GuDiagnosticUsb $packageRoot 'E' } 'Source payload directory link was accepted.'
        Remove-Item -LiteralPath (Join-Path $packageRoot 'USB/linked')
    }
    # The updater's production allowlist is checked before a private module-scope
    # test substitution permits synthetic bytes. No public override is exposed.
    $approvedHashes = @(& $module { $script:ApprovedPreviousDiagnosticSha256 })
    Assert ($approvedHashes.Count -eq 2 -and
        $approvedHashes[0] -ceq '799442355c1de89d704263556c091d4becedad61273c4ce1ba022fd2aadfbac1' -and
        $approvedHashes[1] -ceq 'ba11eb474ee21e5daba2b20018534df24af7544a0b64e5b717f8b54f98acbe2e') 'The exact approved previous release hashes changed.'
    [byte[]]$previousBytes = $fixtureBytes.Clone(); $previousBytes[400] = 4
    $previousHash = Hash $previousBytes
    [byte[]]$secondPreviousBytes = $fixtureBytes.Clone(); $secondPreviousBytes[400] = 41
    $secondPreviousHash = Hash $secondPreviousBytes
    & $module {
        param($FirstHash, $SecondHash, $AlternateBytes)
        $script:ApprovedPreviousDiagnosticSha256 = @($FirstHash, $SecondHash)
        $script:AlternatePreviousBytes = $AlternateBytes
    } $previousHash $secondPreviousHash $secondPreviousBytes
    function Initialize-PreviousUsb([string]$Destination, [byte[]]$PreviousImage=$previousBytes) {
        [IO.Directory]::CreateDirectory((Join-Path $Destination 'EFI/BOOT')) | Out-Null
        [IO.Directory]::CreateDirectory((Join-Path $Destination 'GPU-UNLOCK-ADA/RUN00')) | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $Destination 'EFI/BOOT/BOOTX64.EFI'), $PreviousImage)
        [IO.File]::WriteAllText((Join-Path $Destination 'GPU-UNLOCK-ADA/RUN00/REPORT.TXT'), 'existing hardware report')
        [IO.File]::WriteAllText((Join-Path $Destination 'keep.txt'), 'unrelated user file')
    }
    Reset-Package
    $destination = New-Destination
    Initialize-PreviousUsb $destination
    $updated = Update-GuDiagnosticUsb $packageRoot 'e:'
    Assert ($updated.Root -ceq $destination -and $updated.Sha256 -ceq $fixtureHash) 'Update did not return the verified new image.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $fixtureHash) 'Updated EFI differs from the package.'
    Assert ((Hash ([IO.File]::ReadAllBytes($updated.BackupPath))) -ceq $previousHash) 'Previous EFI backup was not retained exactly.'
    Assert ($updated.PreviousSha256 -ceq $previousHash) 'Update did not identify the exact previous EFI.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'GPU-UNLOCK-ADA/RUN00/REPORT.TXT') -Raw) -ceq 'existing hardware report') 'Update modified the existing report.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'keep.txt') -Raw) -ceq 'unrelated user file') 'Update changed an unrelated user file.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 2) 'Successful update left unexpected temporary files.'
    Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'Updater accepted an already updated EFI as the approved previous release.'

    $destination = New-Destination
    Initialize-PreviousUsb $destination $secondPreviousBytes
    $updated = Update-GuDiagnosticUsb $packageRoot 'E'
    Assert ($updated.Sha256 -ceq $fixtureHash -and $updated.PreviousSha256 -ceq $secondPreviousHash) 'The second approved release was not updated correctly.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $fixtureHash) 'The second approved release did not receive the exact package EFI.'
    Assert ((Hash ([IO.File]::ReadAllBytes($updated.BackupPath))) -ceq $secondPreviousHash) 'The second approved release backup differs from the original.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'GPU-UNLOCK-ADA/RUN00/REPORT.TXT') -Raw) -ceq 'existing hardware report') 'Updating the second approved release modified its report.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'keep.txt') -Raw) -ceq 'unrelated user file') 'Updating the second approved release changed an unrelated file.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 2) 'The second approved release left unexpected temporary files.'

    $destination = New-Destination
    Initialize-PreviousUsb $destination
    [IO.File]::WriteAllText((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'), 'unknown existing EFI')
    Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'An unknown previous EFI was replaced.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Raw) -ceq 'unknown existing EFI') 'Unknown previous EFI changed.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Unknown EFI caused update writes.'

    # Being on the allowlist is insufficient if a different allowed image
    # replaces the one observed at the start of this particular update.
    foreach ($mode in @('approved-boot-before-write','approved-boot-before-commit')) {
        $destination = New-Destination @{} $mode
        Initialize-PreviousUsb $destination
        Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } ('A changed but approved previous EFI was accepted: ' + $mode)
        Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $secondPreviousHash) 'Updater altered a different approved image that appeared during staging.'
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Filter '*.bak' -Force).Count -eq 0) 'Updater moved a different approved image into the backup.'
        Assert ((Get-Content -LiteralPath (Join-Path $destination 'GPU-UNLOCK-ADA/RUN00/REPORT.TXT') -Raw) -ceq 'existing hardware report') 'Approved-image swap damaged the report.'
    }

    $destination = New-Destination @{DiskIsSystem=$true}
    Initialize-PreviousUsb $destination
    Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'Update accepted a system disk.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'System disk rejection still wrote update files.'
    foreach ($mode in @('change-before-write','change-before-commit')) {
        $destination = New-Destination @{} $mode
        Initialize-PreviousUsb $destination
        Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } ('Update accepted changed USB identity: ' + $mode)
        Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $previousHash) 'Identity change modified the previous EFI.'
        Assert ((Get-Content -LiteralPath (Join-Path $destination 'GPU-UNLOCK-ADA/RUN00/REPORT.TXT') -Raw) -ceq 'existing hardware report') 'Identity failure modified an existing report.'
    }
    $destination = New-Destination @{} 'foreign-boot-before-commit'
    Initialize-PreviousUsb $destination
    Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'Update accepted a changed previous EFI before backup.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Raw) -ceq 'foreign boot image') 'Updater moved or overwrote a changed previous EFI.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Filter '*.bak' -Force).Count -eq 0) 'Updater backed up an unapproved foreign EFI.'

    $destination = New-Destination @{} 'foreign-boot-after-backup'
    Initialize-PreviousUsb $destination
    Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'Updater overwrote a late foreign destination after backup.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Raw) -ceq 'foreign boot image') 'Late foreign EFI was changed.'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Filter '*.bak' -Force)
    Assert ($backups.Count -eq 1 -and (Hash ([IO.File]::ReadAllBytes($backups[0].FullName))) -ceq $previousHash) 'Late collision lost the approved previous backup.'
    Assert ((Get-Content -LiteralPath (Join-Path $destination 'GPU-UNLOCK-ADA/RUN00/REPORT.TXT') -Raw) -ceq 'existing hardware report') 'Late collision damaged the report.'

    $destination = New-Destination @{} 'change-after-backup'
    Initialize-PreviousUsb $destination
    Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'Updater accepted changed USB identity after backup.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'))) 'Update installed EFI after USB identity changed.'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Filter '*.bak' -Force)
    Assert ($backups.Count -eq 1 -and (Hash ([IO.File]::ReadAllBytes($backups[0].FullName))) -ceq $previousHash) 'Identity change lost the approved previous backup.'

    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        $destination = New-Destination
        Initialize-PreviousUsb $destination
        $reportTree = Join-Path $destination 'GPU-UNLOCK-ADA'
        $movedReports = Join-Path $destination 'original-reports'
        [IO.Directory]::Move($reportTree, $movedReports)
        New-Item -ItemType SymbolicLink -Path $reportTree -Target $movedReports | Out-Null
        Assert-Throws { Update-GuDiagnosticUsb $packageRoot 'E' } 'Updater accepted a linked report root.'
        Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $previousHash) 'Linked report root rejection changed the EFI.'
        Assert ((Get-Content -LiteralPath (Join-Path $movedReports 'RUN00/REPORT.TXT') -Raw) -ceq 'existing hardware report') 'Linked report root was modified.'
    }
    Write-Output "USB preparation/update tests passed: $script:Checks checks. Storage inventory was mocked; only temporary files were changed."
} finally {
    Remove-Module $module.Name -Force
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
