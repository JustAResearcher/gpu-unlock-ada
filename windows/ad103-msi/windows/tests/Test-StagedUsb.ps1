# SPDX-License-Identifier: GPL-2.0-only
# No hardware, network or Python installation. Synthetic bytes; actual file IO.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$module = Import-Module (Join-Path $PSScriptRoot '../usb-staged/StagedUsb.psm1') -Force -PassThru
$script:Checks = 0
function Assert([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message }; $script:Checks++ }
function Assert-Throws([scriptblock]$Action,[string]$Message) {
    $thrown = $false; try { & $Action | Out-Null } catch { $thrown = $true }
    Assert $thrown $Message
}
function Hash([byte[]]$Bytes) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() } finally { $h.Dispose() }
}
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('gu-staged-usb-tests-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $fixture 'package'; $assets = Join-Path $fixture 'local-assets'
$null = [IO.Directory]::CreateDirectory($package); $null = [IO.Directory]::CreateDirectory($assets)
[byte[]]$image = New-Object byte[] 2048
$image[0]=77;$image[1]=90;$image[60]=128;$image[128]=80;$image[129]=69
$image[132]=100;$image[133]=134;$image[152]=11;$image[153]=2;$image[220]=10
$markers = [Text.Encoding]::ASCII.GetBytes('gpu-unlock-ada Windows preboot port 0.6.4-unlock-test mode=staged-unlock-test STAGED_UNLOCK_INTENT: UNLOCK_VERIFIED: Falcon start booter_load_ad102.bin')
[Array]::Copy($markers,0,$image,512,$markers.Length)
$imageHash = Hash $image
[byte[]]$previous = $image.Clone();$previous[400]=42;$previousHash = Hash $previous
$manifestPath = Join-Path $package 'build-result.json';$imagePath = Join-Path $package 'BOOTX64.EFI'
$productionPins = & $module { $script:AssetPins }
$pins = [ordered]@{}
foreach ($name in $productionPins.Keys) {
    $bytes = [Text.Encoding]::ASCII.GetBytes('synthetic fixture: ' + $name)
    [IO.File]::WriteAllBytes((Join-Path $assets $name),$bytes)
    $pins[$name]=@{bytes=$bytes.Length;sha256=(Hash $bytes)}
}
function Reset-Package {
    [IO.File]::WriteAllBytes($imagePath,$image)
    $board = @{id=0x270510de;subsystem=0xe1301462L;report_sha256=('1'*64)}
    $sources = @('app','app_io','ownership','format','runtime','sha256','checks','port','compat','probe',
                 'capture_pe','rehearsal','rehearsal_handoff','staged_state') | ForEach-Object {'windows/src/' + $_ + '.c'}
    $sources += @('unlock_610','prep_610','layout_610','vbios_610','gfw_eval') | ForEach-Object {'kernel/gpu_unlock/' + $_ + '.c'}
    $required = [ordered]@{};foreach ($name in $pins.Keys) { if ($name -like '*.bin') { $required[$name]=$pins[$name] } }
    $e = @{schema_version=1;source_version='0.5.2-rehearsal';restoration_verified=$true;windows_boot_user_confirmed=$true
           unlock_attempted=$false;app_mmio_writes=0;windows_handoff='pending';profile_tag=('1'*64);report_sha256=('2'*64)}
    $manifest = @{mode='staged-unlock-test';version='0.6.4-unlock-test';hardware_tested=$false;unlock_enabled=$true
        staged_unlock_enabled=$true;assets_verified=$true;helper_protocol=$null;board_profile=$board
        sha256=$imageHash;size=$image.Length;capture_profile=@{board=$board;staged_unlock_evidence=$e}
        required_assets=$required;compiled_sources=$sources
        build_defines=@{GU_ENABLE_UNLOCK=1;GU_STAGED_UNLOCK=1;GU_REHEARSAL=0;GU_SESSION_HELPER=0}}
    [IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 8))
}
function Change-Manifest([string]$Path,$Value) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $parts = $Path.Split('.');$parent = $manifest
    for ($i=0;$i -lt $parts.Length-1;$i++) { $parent = $parent.($parts[$i]) }
    $parent.($parts[-1]) = $Value
    [IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 8))
}
function New-Destination([hashtable]$Overrides=@{},[string]$Mode='') {
    $root = Join-Path $fixture ([Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $root 'EFI/BOOT'))
    $null = [IO.Directory]::CreateDirectory((Join-Path $root 'GPU-UNLOCK-ADA/RUN06'))
    [IO.File]::WriteAllBytes((Join-Path $root 'EFI/BOOT/BOOTX64.EFI'),$previous)
    [IO.File]::WriteAllText((Join-Path $root 'GPU-UNLOCK-ADA/RUN06/REPORT.TXT'),'preserved report')
    [IO.File]::WriteAllText((Join-Path $root 'EFI/BOOT/older-backup.bak'),'preserved backup')
    $existingAssets = Join-Path $root 'GPU-UNLOCK-ADA/ASSETS'
    $null = [IO.Directory]::CreateDirectory($existingAssets)
    foreach ($name in $pins.Keys) {
        $path = Join-Path $existingAssets $name
        [IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes((Join-Path $assets $name)))
        [IO.File]::SetLastWriteTimeUtc($path,[DateTime]::new(2025,1,2,3,4,6,[DateTimeKind]::Utc))
    }
    $snapshot = @{DriveLetter='E';Root=$root;Label='Test USB';DiskNumber=2;DiskUniqueId='disk';DiskPath='usb-path'
        PartitionNumber=1;PartitionOffset=1048576;VolumeUniqueId='volume';VolumeSize=128MB;BusType='USB';FileSystem='FAT32'
        DiskIsBoot=$false;DiskIsSystem=$false;DiskIsReadOnly=$false;DiskIsOffline=$false;PartitionIsBoot=$false
        PartitionIsSystem=$false;FreeBytes=2MB;SystemDrive='C:'}
    foreach ($key in $Overrides.Keys) { $snapshot[$key]=$Overrides[$key] }
    & $module {
        param($Snapshot,$Mode)
        $script:TestSnapshot=$Snapshot;$script:SnapshotMode=$Mode;$script:SnapshotCalls=0
        function script:Get-GuUsbSnapshot([string]$Letter) {
            $script:SnapshotCalls++;$copy=$script:TestSnapshot.PSObject.Copy()
            if ($script:SnapshotMode -eq ('identity-' + $script:SnapshotCalls)) { $copy.DiskUniqueId='different' }
            if ($script:SnapshotMode -eq 'foreign-efi' -and $script:SnapshotCalls -eq 3) {
                [IO.File]::WriteAllText((Join-Path $copy.Root 'EFI/BOOT/BOOTX64.EFI'),'foreign EFI')
            }
            if ($script:SnapshotMode -eq 'corrupt-stage' -and $script:SnapshotCalls -eq 3) {
                $stages = @(Get-ChildItem -LiteralPath (Join-Path $copy.Root 'EFI/BOOT') -Force -Filter '.unlock-image-*.tmp')
                [IO.File]::WriteAllText($stages[0].FullName,'corrupt')
            }
            if ($script:SnapshotMode -eq ('asset-changed-' + $script:SnapshotCalls)) {
                [IO.File]::WriteAllText((Join-Path $copy.Root 'GPU-UNLOCK-ADA/ASSETS/gsp_ga10x.bin'),'foreign asset')
            }
            return $copy
        }
    } ([pscustomobject]$snapshot) $Mode
    return $root
}
function Assert-Preserved([string]$Root) {
    Assert ((Get-Content -LiteralPath (Join-Path $Root 'GPU-UNLOCK-ADA/RUN06/REPORT.TXT') -Raw) -ceq 'preserved report') 'Report changed.'
    Assert ((Get-Content -LiteralPath (Join-Path $Root 'EFI/BOOT/older-backup.bak') -Raw) -ceq 'preserved backup') 'Existing backup changed.'
}
function Assert-AssetsPreserved([string]$Root) {
    $directory=Join-Path $Root 'GPU-UNLOCK-ADA/ASSETS'
    Assert (@(Get-ChildItem -LiteralPath $directory -Force).Count -eq $pins.Count) 'Asset entry count changed.'
    foreach ($name in $pins.Keys) {
        $path=Join-Path $directory $name
        Assert ((Hash ([IO.File]::ReadAllBytes($path))) -ceq $pins[$name].sha256) ('Asset bytes changed: '+$name)
        Assert ([IO.File]::GetLastWriteTimeUtc($path) -eq [DateTime]::new(2025,1,2,3,4,6,[DateTimeKind]::Utc)) ('Asset was rewritten: '+$name)
    }
}
function Assert-OldImage([string]$Root) {
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $Root 'EFI/BOOT/BOOTX64.EFI')))) -ceq $previousHash) 'Old EFI changed before acceptance.'
}
try {
    Assert ((& $module {$script:PreviousSha256}) -ceq 'fe02838ab242f1f785483322f8e85953df1416a42b712b3a5c75d54e495353e4') 'Production old-image allowlist changed.'
    Assert ($productionPins.Count -eq 7 -and $productionPins['gsp_ga10x.bin'].bytes -eq 84277400) 'Production asset pins absent.'
    Assert ($productionPins['NVIDIA-LICENSE'].sha256 -ceq 'c692fb14ad4f499c6aee1b9e144e6905148c420e6225b17cb2937366c1e507ad') 'License pin absent.'
    Assert ($module.ExportedFunctions.Count -eq 2 -and $module.ExportedFunctions.ContainsKey('Update-GuStagedUsb')) 'Private override exported.'
    Assert ($null -eq (& $module {Get-Command Find-GuPython,Get-GuPreparedAssets,Invoke-GuPython -ErrorAction SilentlyContinue})) 'Upgrade still depends on Python preparation.'
    & $module {param($Pins) $script:AssetPins=$Pins} $pins
    Reset-Package;$root=New-Destination
    Assert-Throws { Update-GuStagedUsb $package E } 'Synthetic old EFI accepted by production pin.'
    Assert-OldImage $root
    & $module {param($Hash) $script:PreviousSha256=$Hash} $previousHash
    $result = Update-GuStagedUsb $package 'e:'
    Assert ($result.Sha256 -ceq $imageHash -and $result.Root -ceq $root) 'Wrong update result.'
    Assert ((Hash ([IO.File]::ReadAllBytes((Join-Path $root 'EFI/BOOT/BOOTX64.EFI')))) -ceq $imageHash) 'Wrong installed image.'
    Assert ((Hash ([IO.File]::ReadAllBytes($result.BackupPath))) -ceq $previousHash) 'Wrong previous-image backup.'
    Assert ([IO.Path]::GetFileName($result.BackupPath) -match '^BOOTX64-0\.6\.3-[a-f0-9]{32}\.bak$') 'Wrong released-version backup name.'
    & $module {param($Path) Assert-GuAssets $Path} $result.AssetsPath
    Assert-Preserved $root;Assert-AssetsPreserved $root
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $root 'GPU-UNLOCK-ADA') -Force -Directory).Count -eq 2) 'Upgrade created extra asset/staging directories.'
    Assert-Throws { Update-GuStagedUsb $package E } 'Already-updated USB was overwritten.'
    foreach ($bad in @('','E:/folder','\\server\share','E:*','1')) {
        Assert-Throws { Update-GuStagedUsb $package $bad } 'Invalid drive letter accepted.'
    }
    foreach ($override in @(@{BusType='NVMe'},@{FileSystem='NTFS'},@{DiskIsBoot=$true},@{DiskIsSystem=$true},
        @{PartitionIsBoot=$true},@{PartitionIsSystem=$true},@{DiskIsReadOnly=$true},@{DiskIsOffline=$true},
        @{SystemDrive='E:'},@{DiskIsBoot='False'},@{FreeBytes=1},@{VolumeUniqueId=''},@{DiskUniqueId='';DiskPath=''})) {
        $root=New-Destination $override
        Assert-Throws { Update-GuStagedUsb $package E } 'Unsafe volume accepted.'
        Assert-OldImage $root;Assert-Preserved $root
        Assert-AssetsPreserved $root
    }
    $badFields = @{'mode'='firmware-rehearsal';'version'='0.5.2-rehearsal';'hardware_tested'=$true
        'unlock_enabled'=$false;'staged_unlock_enabled'='true';'assets_verified'=$false;'helper_protocol'='gu-session-v1'
        'sha256'=('0'*64);'size'=2049;'board_profile.id'=0x278210de;'board_profile.subsystem'=1
        'build_defines.GU_REHEARSAL'=1;'build_defines.GU_ENABLE_UNLOCK'=$true
        'capture_profile.staged_unlock_evidence.schema_version'=$true
        'capture_profile.staged_unlock_evidence.windows_boot_user_confirmed'=$false
        'capture_profile.staged_unlock_evidence.unlock_attempted'=$true
        'capture_profile.staged_unlock_evidence.app_mmio_writes'=1
        'capture_profile.staged_unlock_evidence.profile_tag'=('3'*64)
        'compiled_sources'=@('kernel/gpu_unlock/unlock_610.c')}
    foreach ($field in $badFields.Keys) {
        Reset-Package;Change-Manifest $field $badFields[$field];$root=New-Destination
        Assert-Throws { Update-GuStagedUsb $package E } ('Invalid manifest accepted: '+$field)
        Assert-OldImage $root;Assert-Preserved $root
    }
    foreach ($oldVersion in @('0.6.0-unlock-test','0.6.1-unlock-test','0.6.2-unlock-test','0.6.3-unlock-test')) {
        Reset-Package;Change-Manifest 'version' $oldVersion;$root=New-Destination
        Assert-Throws { Update-GuStagedUsb $package E } ('Stale package accepted: '+$oldVersion)
        Assert-OldImage $root;Assert-Preserved $root
    }
    Reset-Package
    foreach ($mode in @('identity-2','identity-3','identity-4','identity-5','foreign-efi','corrupt-stage','asset-changed-3','asset-changed-4')) {
        $root=New-Destination @{} $mode
        Assert-Throws { Update-GuStagedUsb $package E } ('Changed state accepted: '+$mode)
        Assert-Preserved $root
        if ($mode -notin @('identity-4','identity-5','foreign-efi','asset-changed-4')) { Assert-OldImage $root }
        if ($mode -in @('identity-4','identity-5','asset-changed-4')) {
            $backups=@(Get-ChildItem -LiteralPath (Join-Path $root 'EFI/BOOT') -Filter 'BOOTX64-0.6.3-*.bak')
            Assert ($backups.Count -eq 1 -and (Hash ([IO.File]::ReadAllBytes($backups[0].FullName))) -ceq $previousHash) 'Backup lost after state change.'
        }
        if ($mode -notlike 'asset-changed-*') {Assert-AssetsPreserved $root}
        else {Assert ((Get-Content -LiteralPath (Join-Path $root 'GPU-UNLOCK-ADA/ASSETS/gsp_ga10x.bin') -Raw) -ceq 'foreign asset') 'Updater rewrote changed assets.'}
    }
    foreach ($case in @('missing-directory','missing-file','size','hash','extra','subdirectory')) {
        $root=New-Destination;$existingAssets=Join-Path $root 'GPU-UNLOCK-ADA/ASSETS';$path=Join-Path $existingAssets 'gsp_ga10x.bin'
        switch ($case) {
            'missing-directory' {Remove-Item -LiteralPath $existingAssets -Recurse -Force}
            'missing-file' {Remove-Item -LiteralPath $path}
            'size' {[IO.File]::WriteAllText($path,'wrong size')}
            'hash' {$changed=[IO.File]::ReadAllBytes($path);$changed[0]=$changed[0] -bxor 1;[IO.File]::WriteAllBytes($path,$changed)}
            'extra' {[IO.File]::WriteAllText((Join-Path $existingAssets 'unexpected.bin'),'extra')}
            'subdirectory' {Remove-Item -LiteralPath $path;$null=[IO.Directory]::CreateDirectory($path)}
        }
        Assert-Throws { Update-GuStagedUsb $package E } ('Invalid existing assets accepted: '+$case)
        Assert-OldImage $root;Assert-Preserved $root
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $root 'EFI/BOOT') -Force).Count -eq 2) 'Invalid existing assets caused USB staging.'
    }
    foreach ($script in @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '../usb-staged') -Filter '*.ps1')) {
        $tokens=$null;$errors=$null;[Management.Automation.Language.Parser]::ParseFile($script.FullName,[ref]$tokens,[ref]$errors) | Out-Null
        Assert ($errors.Count -eq 0) ('PowerShell syntax error: '+$script.Name)
    }
    Write-Host ('Staged USB checks passed: '+$script:Checks+'; Windows storage mocked; synthetic assets only.')
} finally { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue }
