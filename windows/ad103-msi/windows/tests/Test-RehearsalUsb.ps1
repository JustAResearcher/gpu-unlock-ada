# SPDX-License-Identifier: GPL-2.0-only
# Synthetic bytes and temporary files only; Windows storage discovery is mocked.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$module = Import-Module (Join-Path $PSScriptRoot '../rehearsal/RehearsalUsb.psm1') -Force -PassThru
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
    $h = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($h.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $h.Dispose() }
}
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('gu-rehearsal-usb-tests-' + [Guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $fixture 'package'
$imagePath = Join-Path $packageRoot 'USB/EFI/BOOT/BOOTX64.EFI'
$manifestPath = Join-Path $packageRoot 'build-result.json'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($imagePath)) | Out-Null
# Structural PE fixture; it is not an executable firmware application.
[byte[]]$fixtureBytes = New-Object byte[] 1024
$fixtureBytes[0] = 77; $fixtureBytes[1] = 90; $fixtureBytes[60] = 128
$fixtureBytes[128] = 80; $fixtureBytes[129] = 69
$fixtureBytes[132] = 100; $fixtureBytes[133] = 134
$fixtureBytes[152] = 11; $fixtureBytes[153] = 2; $fixtureBytes[220] = 10
$marker = [Text.Encoding]::ASCII.GetBytes('REHEARSAL_ONLY: firmware-rehearsal Windows preboot port 0.5.2-rehearsal REHEARSAL_GOP_DEFERRED')
[Array]::Copy($marker, 0, $fixtureBytes, 512, $marker.Length)
$fixtureHash = Hash $fixtureBytes
[byte[]]$previousBytes = $fixtureBytes.Clone(); $previousBytes[400] = 42
$previousHash = Hash $previousBytes
[byte[]]$secondPreviousBytes = $fixtureBytes.Clone(); $secondPreviousBytes[400] = 43
$secondPreviousHash = Hash $secondPreviousBytes
[byte[]]$thirdPreviousBytes = $fixtureBytes.Clone(); $thirdPreviousBytes[400] = 44
$thirdPreviousHash = Hash $thirdPreviousBytes
$compiled = @('app','app_io','ownership','format','runtime','sha256','checks','port','compat',
              'probe','capture_pe','rehearsal','rehearsal_handoff') |
    ForEach-Object { 'windows/src/' + $_ + '.c' }
$compiled += @('kernel/gpu_unlock/vbios_610.c','kernel/gpu_unlock/gfw_eval.c')
function Reset-Package {
    [IO.File]::WriteAllBytes($imagePath, $fixtureBytes)
    $board = @{id=0x270510de; subsystem=0xe1301462L; report_sha256=('1'*64)}
    $info = @{version=0; width=2560; height=1440; pixel_format=1; stride=4096
              masks=@(16711680,65280,255,4278190080L)}
    $evidence = @{
        schema_version=1; source_version='0.5.1-rehearsal'; report_sha256=('2'*64)
        profile_tag=$board.report_sha256; observed_stage=4; unlock_attempted=$false; restoration_completed=$false
        baseline=@{max_mode=14; mode=0; info_bytes=36; framebuffer_base=446676598784L; framebuffer_bytes=23592960; info=$info}
        deferred=@{max_mode=14; mode=15; info_bytes=36; framebuffer_base=0; framebuffer_bytes=0; info=$info.Clone()}
    }
    $manifest = [ordered]@{
        mode='firmware-rehearsal'; version='0.5.2-rehearsal'; hardware_tested=$false
        unlock_enabled=$false; helper_protocol=$null; sha256=$fixtureHash; size=$fixtureBytes.Length
        board_profile=$board; capture_profile=@{board=$board; unlock_authorized=$false; restoration_evidence=$evidence}
        compiled_sources=$compiled
    }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}
function Set-Manifest([string]$Name, $Value) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.$Name = $Value
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}
function Set-Evidence([string]$Path, $Value) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $parts = $Path.Split('.')
    $parent = $manifest.capture_profile.restoration_evidence
    for ($i = 0; $i -lt $parts.Length - 1; $i++) { $parent = $parent.($parts[$i]) }
    $parent.($parts[-1]) = $Value
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}
function New-Destination([hashtable]$Overrides=@{}, [string]$Mode='') {
    $destination = Join-Path $fixture ([Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory((Join-Path $destination 'EFI/BOOT')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $destination 'GPU-UNLOCK-ADA/RUN02')) | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'), $previousBytes)
    [IO.File]::WriteAllText((Join-Path $destination 'GPU-UNLOCK-ADA/RUN02/REPORT.TXT'), 'previous report')
    [IO.File]::WriteAllText((Join-Path $destination 'GPU-UNLOCK-ADA/RUN02/TOPOLOGY.TXT'), 'previous capture')
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
                ($script:SnapshotMode -eq 'change-before-backup' -and $script:SnapshotCalls -eq 3) -or
                ($script:SnapshotMode -eq 'change-after-backup' -and $script:SnapshotCalls -eq 4)) {
                $value.DiskUniqueId = 'a-different-disk'
            }
            if (($script:SnapshotMode -eq 'foreign-before-backup' -and $script:SnapshotCalls -eq 3) -or
                ($script:SnapshotMode -eq 'foreign-after-backup' -and $script:SnapshotCalls -eq 4)) {
                [IO.File]::WriteAllText((Join-Path $value.Root 'EFI/BOOT/BOOTX64.EFI'), 'foreign boot image')
            }
            if ($script:SnapshotMode -eq 'staged-after-backup' -and $script:SnapshotCalls -eq 4) {
                $tmp = @(Get-ChildItem -LiteralPath (Join-Path $value.Root 'EFI/BOOT') -Filter '*.tmp' -Force)
                [IO.File]::WriteAllText($tmp[0].FullName, 'foreign staged image')
            }
            return $value
        }
    } ([pscustomobject]$values) $Mode
    return $destination
}
function Assert-Reports([string]$Destination) {
    Assert ((Get-Content -LiteralPath (Join-Path $Destination 'GPU-UNLOCK-ADA/RUN02/REPORT.TXT') -Raw) -ceq 'previous report') 'Report was modified.'
    Assert ((Get-Content -LiteralPath (Join-Path $Destination 'GPU-UNLOCK-ADA/RUN02/TOPOLOGY.TXT') -Raw) -ceq 'previous capture') 'Capture was modified.'
}
try {
    $productionHashes = @(& $module { $script:ApprovedPreviousSha256 })
    Assert ($productionHashes.Count -eq 3) 'Unexpected released EFI allowlist size.'
    Assert ($productionHashes -ccontains '0f4a9c0fd525600c1b79b03d896c6d454a073f826b8625216ceafb5367cf5df5') 'Released 0.4.2 hash missing.'
    Assert ($productionHashes -ccontains 'ab47a2a69e01152d062369657521a76e97e7cc843cff1af0d64553d4e0526a2d') 'Released 0.5.0 hash missing.'
    Assert ($productionHashes -ccontains '05bb134f27d3c99063e94802b37848de9d1673955daa2394c341831171f86cc3') 'Released 0.5.1 hash missing.'
    $exports = @($module.ExportedFunctions.Keys)
    Assert ($exports.Count -eq 2 -and $exports -contains 'Update-GuRehearsalUsb' -and $exports -contains 'Get-GuRehearsalUsbCandidates') 'Private helper or policy override exported.'
    Reset-Package
    $destination = New-Destination
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Synthetic old EFI accepted by production allowlist.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $previousHash) 'Rejected old EFI changed.'
    # Check each production allowlist entry through the actual destination guard.
    # Only the hash computation is mocked here; ordinary path checks still run.
    $probe = {
        param($Root, $Hash, $Expected)
        $original = (Get-Command Get-GuBytesHash).ScriptBlock
        try {
            $script:ProbeHash = $Hash
            function script:Get-GuBytesHash { param([byte[]]$Bytes) return $script:ProbeHash }
            Assert-GuPreviousRehearsalDestination $Root $Expected
        } finally {
            Set-Item Function:script:Get-GuBytesHash -Value $original
            Remove-Variable ProbeHash -Scope Script
        }
    }
    foreach ($releasedHash in $productionHashes) {
        Assert ((& $module $probe $destination $releasedHash $releasedHash) -ceq $releasedHash) 'Released image was refused.'
        Assert-Throws { & $module $probe $destination $releasedHash ('0'*64) } 'A changed released image passed the expected-hash check.'
    }
    foreach ($unknownHash in @(('0'*64), $fixtureHash, $productionHashes[0].ToUpperInvariant())) {
        Assert-Throws { & $module $probe $destination $unknownHash '' } 'An unrecognized or noncanonical old-image hash was accepted.'
    }
    # Private test substitution permits three synthetic fixtures, never public API.
    & $module { param($First, $Second, $Third) $script:ApprovedPreviousSha256 = @($First, $Second, $Third) } $previousHash $secondPreviousHash $thirdPreviousHash
    $updated = Update-GuRehearsalUsb $packageRoot 'e:'
    Assert ($updated.Root -ceq $destination -and $updated.Sha256 -ceq $fixtureHash) 'Successful update result wrong.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $fixtureHash) 'Installed image differs.'
    Assert ((Hash ([IO.File]::ReadAllBytes($updated.BackupPath))) -ceq $previousHash) 'Backup differs.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 2) 'Successful update left extra temporary files.'
    Assert-Reports $destination
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Second update overwrote the rehearsal image.'

    $destination = New-Destination
    [IO.File]::WriteAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'), $secondPreviousBytes)
    $updated = Update-GuRehearsalUsb $packageRoot E
    Assert ($updated.PreviousSha256 -ceq $secondPreviousHash) 'Second allowed old image was not selected.'
    Assert ((Hash ([IO.File]::ReadAllBytes($updated.BackupPath))) -ceq $secondPreviousHash) 'Second allowed old-image backup differs.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $fixtureHash) 'Second allowed old-image update differs.'
    Assert-Reports $destination
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Repeated update from second old image was accepted.'

    $destination = New-Destination
    [IO.File]::WriteAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'), $thirdPreviousBytes)
    $updated = Update-GuRehearsalUsb $packageRoot E
    Assert ($updated.PreviousSha256 -ceq $thirdPreviousHash) 'Third allowed old image was not selected.'
    Assert ((Hash ([IO.File]::ReadAllBytes($updated.BackupPath))) -ceq $thirdPreviousHash) 'Third allowed old-image backup differs.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $destination 'EFI/BOOT/BOOTX64.EFI')))) -ceq $fixtureHash) 'Third allowed old-image update differs.'
    Assert-Reports $destination
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Repeated update from third old image was accepted.'

    foreach ($bad in @('', 'E:/folder', '\\server\share', 'E:*', '1')) {
        Assert-Throws { Update-GuRehearsalUsb $packageRoot $bad } ('Invalid drive accepted: ' + $bad)
    }
    foreach ($override in @(@{BusType='NVMe'}, @{FileSystem='NTFS'}, @{FileSystem='exFAT'},
        @{DiskIsBoot=$true}, @{DiskIsSystem=$true}, @{PartitionIsBoot=$true}, @{PartitionIsSystem=$true},
        @{DiskIsReadOnly=$true}, @{DiskIsOffline=$true}, @{DiskIsBoot='False'}, @{SystemDrive='E:'},
        @{FreeBytes=1000}, @{VolumeUniqueId=''}, @{DiskUniqueId='';DiskPath=''})) {
        $destination = New-Destination $override
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Unsafe volume accepted: ' + ($override.Keys -join ','))
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Rejected disk was staged or changed.'
        Assert-Reports $destination
    }
    $badFields = @{
        mode='diagnostic'; version='0.4.2-diagnostic'; hardware_tested='false'; unlock_enabled=$true
        helper_protocol='gu-session-v1'; board_profile=$null; capture_profile=$null; sha256=('0'*64)
        size=1025; compiled_sources=@('kernel/gpu_unlock/unlock_610.c')
    }
    foreach ($field in $badFields.Keys) {
        Reset-Package; $destination = New-Destination
        Set-Manifest $field $badFields[$field]
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Bad manifest accepted: ' + $field)
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Invalid package modified disk.'
    }
    foreach ($obsoleteVersion in @('0.5.0-rehearsal','0.5.1-rehearsal')) {
        Reset-Package; $destination = New-Destination
        Set-Manifest version $obsoleteVersion
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Obsolete rehearsal package was accepted as the new payload.'
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Obsolete package modified disk.'
    }
    $badEvidence = @{
        schema_version=2; source_version='0.5.0-rehearsal'; report_sha256='not-a-digest'; profile_tag=('3'*64)
        observed_stage=5; unlock_attempted=$true; restoration_completed=$true
        'baseline.max_mode'=0; 'baseline.mode'=14; 'baseline.info_bytes'=35
        'baseline.framebuffer_base'=0; 'baseline.framebuffer_bytes'=10
        'baseline.info.width'=0; 'baseline.info.height'=0; 'baseline.info.pixel_format'=2; 'baseline.info.stride'=2559
        'baseline.info.masks'=@(0,0,0); 'deferred.max_mode'=13; 'deferred.mode'=14
        'deferred.info_bytes'=40; 'deferred.framebuffer_base'=1; 'deferred.framebuffer_bytes'=1
        'deferred.info.width'=1920; 'deferred.info.masks'=@(0,0,0,0)
    }
    foreach ($path in $badEvidence.Keys) {
        Reset-Package; $destination = New-Destination
        Set-Evidence $path $badEvidence[$path]
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Invalid restoration evidence accepted: ' + $path)
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Invalid restoration evidence modified disk.'
    }
    foreach ($case in @('absent','null','wrong-profile','unlock-authorized','boolean-mode','string-mode','overflow-mode','negative-mode','boolean-mask','missing-info')) {
        Reset-Package; $destination = New-Destination
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        switch ($case) {
            'absent' { $manifest.capture_profile.PSObject.Properties.Remove('restoration_evidence') }
            'null' { $manifest.capture_profile.restoration_evidence = $null }
            'wrong-profile' { $manifest.capture_profile.board.report_sha256 = ('4'*64) }
            'unlock-authorized' { $manifest.capture_profile.unlock_authorized = $true }
            'boolean-mode' { $manifest.capture_profile.restoration_evidence.deferred.mode = $true }
            'string-mode' { $manifest.capture_profile.restoration_evidence.deferred.mode = '15' }
            'overflow-mode' { $manifest.capture_profile.restoration_evidence.deferred.mode = 4294967296L }
            'negative-mode' { $manifest.capture_profile.restoration_evidence.deferred.mode = -1 }
            'boolean-mask' { $manifest.capture_profile.restoration_evidence.deferred.info.masks[0] = $true }
            'missing-info' { $manifest.capture_profile.restoration_evidence.deferred.PSObject.Properties.Remove('info') }
        }
        [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Malformed restoration evidence accepted: ' + $case)
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $destination 'EFI/BOOT') -Force).Count -eq 1) 'Malformed restoration evidence modified disk.'
    }
    foreach ($field in @('mode','version','hardware_tested','unlock_enabled','board_profile','capture_profile','compiled_sources','size','sha256')) {
        Reset-Package; $destination = New-Destination
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $manifest.PSObject.Properties.Remove($field)
        [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8))
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Missing manifest field accepted: ' + $field)
    }
    Reset-Package
    $destination = New-Destination
    Set-Manifest board_profile ([pscustomobject]@{id=0x278210de;subsystem=0xe1301462L})
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Different GPU accepted.'
    Reset-Package
    Set-Manifest compiled_sources (@($compiled[0]) * $compiled.Count)
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Duplicated compiled source inventory accepted.'
    Reset-Package
    [IO.File]::WriteAllText((Join-Path $packageRoot 'USB/extra.sys'), 'unexpected payload')
    Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Extra USB payload accepted.'
    Remove-Item -LiteralPath (Join-Path $packageRoot 'USB/extra.sys')
    foreach ($case in @('bad-pe','missing-marker','missing-deferred-marker','obsolete-version-marker','unlock-marker','stale-hash')) {
        Reset-Package
        $altered = $fixtureBytes.Clone()
        if ($case -eq 'bad-pe') { $altered[0] = 0 }
        if ($case -eq 'missing-marker') { $altered[512] = 0 }
        if ($case -eq 'missing-deferred-marker') {
            $offset = [Text.Encoding]::ASCII.GetString($altered).IndexOf('REHEARSAL_GOP_DEFERRED')
            $altered[$offset] = 0
        }
        if ($case -eq 'obsolete-version-marker') {
            $obsolete = [Text.Encoding]::ASCII.GetBytes('REHEARSAL_ONLY: firmware-rehearsal Windows preboot port 0.5.1-rehearsal')
            [Array]::Copy($obsolete, 0, $altered, 512, $obsolete.Length)
        }
        if ($case -eq 'unlock-marker') {
            $extra = [Text.Encoding]::ASCII.GetBytes('Press U once')
            [Array]::Copy($extra, 0, $altered, 700, $extra.Length)
        }
        if ($case -eq 'stale-hash') { $altered[900] = 4 }
        [IO.File]::WriteAllBytes($imagePath, $altered)
        if ($case -ne 'stale-hash') { Set-Manifest sha256 (Hash $altered) }
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Invalid EFI accepted: ' + $case)
    }
    foreach ($forbidden in @('Falcon start','unlock_transaction_dma','UNLOCK_SUCCESS','UNLOCK_VERIFIED:',
                              'booter_load_ad102.bin','GUSSN001','DIAGNOSTIC_ONLY:')) {
        Reset-Package
        $altered = $fixtureBytes.Clone()
        $extra = [Text.Encoding]::ASCII.GetBytes($forbidden)
        [Array]::Copy($extra, 0, $altered, 700, $extra.Length)
        [IO.File]::WriteAllBytes($imagePath, $altered)
        Set-Manifest sha256 (Hash $altered)
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Forbidden image marker accepted: ' + $forbidden)
    }
    Reset-Package
    foreach ($mode in @('change-before-write','change-before-backup','change-after-backup',
                        'foreign-before-backup','foreign-after-backup','staged-after-backup')) {
        $destination = New-Destination @{} $mode
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } ('Late change accepted: ' + $mode)
        Assert-Reports $destination
        if ($mode -like 'foreign-*') {
            Assert ((Get-Content -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI') -Raw) -ceq 'foreign boot image') 'Late foreign bootloader was overwritten.'
        }
        if ($mode -in @('change-after-backup','staged-after-backup')) {
            Assert (-not (Test-Path -LiteralPath (Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'))) 'New bootable image committed after failed check.'
        }
    }
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        $destination = New-Destination
        $foreign = Join-Path $fixture 'foreign'; [IO.Directory]::CreateDirectory($foreign) | Out-Null
        New-Item -ItemType SymbolicLink -Path (Join-Path $packageRoot 'USB/linked') -Target $foreign | Out-Null
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Source payload directory link accepted.'
        Remove-Item -LiteralPath (Join-Path $packageRoot 'USB/linked')
        $image = Join-Path $destination 'EFI/BOOT/BOOTX64.EFI'
        Remove-Item -LiteralPath $image
        $target = Join-Path $foreign 'old.efi'; [IO.File]::WriteAllBytes($target, $previousBytes)
        New-Item -ItemType SymbolicLink -Path $image -Target $target | Out-Null
        Assert-Throws { Update-GuRehearsalUsb $packageRoot E } 'Destination image link accepted.'
        Assert ((Hash ([IO.File]::ReadAllBytes($target))) -ceq $previousHash) 'Link target changed.'
    }
    Write-Output ('Rehearsal USB checks passed: ' + $script:Checks + ' (Windows storage mocked).')
} finally {
    Remove-Module $module -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
