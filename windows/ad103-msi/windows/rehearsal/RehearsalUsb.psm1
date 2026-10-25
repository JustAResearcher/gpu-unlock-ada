# SPDX-License-Identifier: GPL-2.0-only
# Active firmware rehearsal: a separate policy from the read-only diagnostic.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$storagePath = Join-Path $PSScriptRoot 'UsbPreparation.psm1'
if (-not (Test-Path -LiteralPath $storagePath -PathType Leaf)) {
    $storagePath = Join-Path $PSScriptRoot '../UsbPreparation.psm1'
}
$script:StorageModule = Import-Module $storagePath -Force -PassThru
$script:ApprovedPreviousSha256 = @(
    '0f4a9c0fd525600c1b79b03d896c6d454a073f826b8625216ceafb5367cf5df5' # diagnostic 0.4.2
    'ab47a2a69e01152d062369657521a76e97e7cc843cff1af0d64553d4e0526a2d' # rehearsal 0.5.0
    '05bb134f27d3c99063e94802b37848de9d1673955daa2394c341831171f86cc3' # rehearsal 0.5.1
)

# Reuse storage guards without changing the diagnostic module's manifest rules
# or approved-old-image list. These helpers are private to this module.
function Assert-GuOrdinaryPath([string]$Path) {
    & $script:StorageModule { param($p) Assert-GuOrdinaryPath $p } $Path
}
function Get-GuBytesHash([byte[]]$Bytes) {
    & $script:StorageModule { param($b) Get-GuBytesHash $b } $Bytes
}
function Read-GuBoundedFile([string]$Path, [long]$MaximumBytes) {
    $bytes = & $script:StorageModule { param($p,$n) Read-GuBoundedFile $p $n } $Path $MaximumBytes
    return ,$bytes
}
function Get-GuUsbSnapshot([string]$DriveLetter) {
    & $script:StorageModule { param($letter) Get-GuUsbSnapshot $letter } $DriveLetter
}
function Assert-GuUsbSnapshot($Snapshot, [long]$RequiredBytes) {
    & $script:StorageModule { param($s,$n) Assert-GuUsbSnapshot $s $n } $Snapshot $RequiredBytes
}
function Assert-GuSameUsb($Before, $After, [long]$RequiredBytes) {
    & $script:StorageModule { param($a,$b,$n) Assert-GuSameUsb $a $b $n } $Before $After $RequiredBytes
}
function Get-GuRehearsalUsbCandidates {
    & $script:StorageModule { Get-GuDiagnosticUsbCandidates }
}

function Test-GuUnsignedInteger($Value, [UInt64]$Maximum) {
    return (($Value -is [int] -or $Value -is [long]) -and $Value -ge 0 -and $Value -le $Maximum)
}

function Assert-GuRestorationEvidence($Manifest) {
    # Build/package validation binds the generated C profile to this evidence.
    # The updater also refuses a missing, stale or malformed evidence record.
    $capture = $Manifest.capture_profile
    $evidence = $capture.restoration_evidence
    if ($null -eq $evidence -or
        -not (Test-GuUnsignedInteger $evidence.schema_version 1) -or $evidence.schema_version -ne 1 -or
        $evidence.source_version -cne '0.5.1-rehearsal' -or
        -not (Test-GuUnsignedInteger $evidence.observed_stage 4) -or $evidence.observed_stage -ne 4 -or
        $evidence.unlock_attempted -isnot [bool] -or $evidence.unlock_attempted -ne $false -or
        $evidence.restoration_completed -isnot [bool] -or $evidence.restoration_completed -ne $false -or
        $capture.unlock_authorized -isnot [bool] -or $capture.unlock_authorized -ne $false -or
        $evidence.report_sha256 -isnot [string] -or $evidence.report_sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        $evidence.profile_tag -isnot [string] -or $evidence.profile_tag -cnotmatch '^[a-f0-9]{64}$' -or
        $evidence.profile_tag -cne $capture.board.report_sha256 -or
        $evidence.profile_tag -cne $Manifest.board_profile.report_sha256) {
        throw 'Missing or inconsistent captured stage-4 restoration evidence.'
    }
    $baseline = $evidence.baseline
    $deferred = $evidence.deferred
    foreach ($state in @($baseline, $deferred)) {
        foreach ($field in @('max_mode','mode','info_bytes')) {
            if (-not (Test-GuUnsignedInteger $state.$field 4294967295)) {
                throw ('Invalid restoration evidence integer: ' + $field)
            }
        }
        foreach ($field in @('framebuffer_base','framebuffer_bytes')) {
            if (-not (Test-GuUnsignedInteger $state.$field ([long]::MaxValue))) {
                throw ('Invalid restoration evidence integer: ' + $field)
            }
        }
        foreach ($field in @('version','width','height','pixel_format','stride')) {
            if (-not (Test-GuUnsignedInteger $state.info.$field 4294967295)) {
                throw ('Invalid restoration display information: ' + $field)
            }
        }
        $masks = @($state.info.masks)
        if ($masks.Count -ne 4) { throw 'Restoration display masks are incomplete.' }
        foreach ($mask in $masks) {
            if (-not (Test-GuUnsignedInteger $mask 4294967295)) { throw 'Invalid restoration display mask.' }
        }
    }
    if ($baseline.max_mode -lt 1 -or $baseline.max_mode -eq 4294967295 -or
        $baseline.mode -ge $baseline.max_mode -or $baseline.info_bytes -lt 36 -or
        $baseline.framebuffer_base -eq 0 -or $baseline.framebuffer_bytes -eq 0 -or
        $baseline.info.width -eq 0 -or $baseline.info.height -eq 0 -or
        $baseline.info.pixel_format -gt 1 -or $baseline.info.stride -lt $baseline.info.width -or
        [decimal]$baseline.info.stride * [decimal]$baseline.info.height -gt [decimal]$baseline.framebuffer_bytes / 4 -or
        $deferred.max_mode -ne $baseline.max_mode -or $deferred.mode -ne ($baseline.max_mode + 1) -or
        $deferred.info_bytes -ne $baseline.info_bytes -or $deferred.framebuffer_base -ne 0 -or
        $deferred.framebuffer_bytes -ne 0) {
        throw 'Restoration evidence does not describe the observed deferred display state.'
    }
    foreach ($field in @('version','width','height','pixel_format','stride')) {
        if ($deferred.info.$field -ne $baseline.info.$field) {
            throw 'Restoration evidence changes the captured display information.'
        }
    }
    for ($i = 0; $i -lt 4; $i++) {
        if ($deferred.info.masks[$i] -ne $baseline.info.masks[$i]) {
            throw 'Restoration evidence changes the captured display masks.'
        }
    }
}

function Get-GuRehearsalPackage([string]$PackageDirectory) {
    $root = [IO.Path]::GetFullPath($PackageDirectory)
    Assert-GuOrdinaryPath $root
    $usb = Join-Path $root 'USB'
    $image = [IO.Path]::GetFullPath((Join-Path $usb 'EFI/BOOT/BOOTX64.EFI'))
    Assert-GuOrdinaryPath $image
    $allItems = @(Get-ChildItem -LiteralPath $usb -Force -Recurse -ErrorAction Stop)
    foreach ($item in $allItems) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'USB payload contains a symbolic link or reparse point.'
        }
    }
    $files = @($allItems | Where-Object { -not $_.PSIsContainer })
    if ($files.Count -ne 1 -or $files[0].FullName -cne $image) {
        throw 'The rehearsal USB payload must contain only EFI/BOOT/BOOTX64.EFI.'
    }
    $manifestBytes = Read-GuBoundedFile (Join-Path $root 'build-result.json') 1048576
    $manifest = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
    if ($manifest.mode -cne 'firmware-rehearsal' -or $manifest.version -cne '0.5.2-rehearsal' -or
        $manifest.hardware_tested -isnot [bool] -or $manifest.hardware_tested -ne $false -or
        $manifest.unlock_enabled -isnot [bool] -or $manifest.unlock_enabled -ne $false -or
        $null -ne $manifest.helper_protocol -or $null -eq $manifest.board_profile -or
        $null -eq $manifest.capture_profile -or
        $manifest.board_profile.id -ne 0x270510de -or
        $manifest.board_profile.subsystem -ne 0xe1301462L -or
        $manifest.sha256 -isnot [string] -or $manifest.sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        ($manifest.size -isnot [int] -and $manifest.size -isnot [long]) -or
        $manifest.size -lt 512 -or $manifest.size -gt 8388608) {
        throw 'Not a complete profile-bound 0.5.2 firmware rehearsal manifest with unlocking disabled.'
    }
    Assert-GuRestorationEvidence $manifest
    $expected = @('app','app_io','ownership','format','runtime','sha256','checks','port','compat',
                  'probe','capture_pe','rehearsal','rehearsal_handoff') |
        ForEach-Object { 'windows/src/' + $_ + '.c' }
    $expected += @('kernel/gpu_unlock/vbios_610.c','kernel/gpu_unlock/gfw_eval.c')
    $compiled = @($manifest.compiled_sources)
    if ($compiled.Count -ne $expected.Count -or @($compiled | Select-Object -Unique).Count -ne $expected.Count) {
        throw 'Unexpected compiled source inventory in the rehearsal manifest.'
    }
    foreach ($source in $compiled) {
        if ($source -isnot [string] -or $expected -cnotcontains $source) {
            throw 'The rehearsal must exclude unlock, payload, session and capture transaction sources.'
        }
    }
    $bytes = Read-GuBoundedFile $image 8388608
    if ($bytes.Length -ne $manifest.size -or (Get-GuBytesHash $bytes) -cne $manifest.sha256) {
        throw 'Rehearsal EFI checksum or size mismatch. Extract the original ZIP again.'
    }
    $pe = [BitConverter]::ToUInt32($bytes, 60)
    if ($bytes[0] -ne 77 -or $bytes[1] -ne 90 -or $pe -gt ($bytes.Length - 94) -or
        [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b -or
        [BitConverter]::ToUInt16($bytes, $pe + 92) -ne 10) {
        throw 'Rehearsal payload is not an x64 UEFI application.'
    }
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    if (-not $text.Contains('REHEARSAL_ONLY:') -or -not $text.Contains('firmware-rehearsal') -or
        -not $text.Contains('Windows preboot port 0.5.2-rehearsal') -or
        -not $text.Contains('REHEARSAL_GOP_DEFERRED')) {
        throw 'Rehearsal identity markers are absent from the EFI.'
    }
    foreach ($forbidden in @('Falcon start','unlock_transaction_dma','UNLOCK_SUCCESS','UNLOCK_VERIFIED:','Press U once',
                              'booter_load_ad102.bin','GUSSN001','DIAGNOSTIC_ONLY:')) {
        if ($text.Contains($forbidden)) { throw ('Unexpected payload marker: ' + $forbidden) }
    }
    return [pscustomobject]@{ Bytes=$bytes; Sha256=$manifest.sha256; Size=$bytes.Length }
}

function Assert-GuPreviousRehearsalDestination([string]$Root, [string]$ExpectedSha256) {
    foreach ($path in @($Root, (Join-Path $Root 'EFI'), (Join-Path $Root 'EFI/BOOT'),
                        (Join-Path $Root 'GPU-UNLOCK-ADA'))) {
        Assert-GuOrdinaryPath $path
        $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
        if (-not $item.PSIsContainer) { throw ('Expected an existing ordinary directory: ' + $path) }
    }
    $image = Join-Path $Root 'EFI/BOOT/BOOTX64.EFI'
    $previousHash = Get-GuBytesHash (Read-GuBoundedFile $image 8388608)
    if ($script:ApprovedPreviousSha256 -cnotcontains $previousHash) {
        throw 'This USB does not contain an exact released 0.4.2 diagnostic, 0.5.0 rehearsal or 0.5.1 rehearsal. Its existing EFI was not changed. Keep the error and do not replace an unrecognized bootloader manually.'
    }
    if ($ExpectedSha256 -and $previousHash -cne $ExpectedSha256) {
        throw 'The previous EFI changed during the update. No further files will be copied.'
    }
    return $previousHash
}

function Update-GuRehearsalUsb {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$PackageDirectory,
          [Parameter(Mandatory=$true)][string]$DriveLetter)
    $DriveLetter = $DriveLetter.Trim()
    if ($DriveLetter -notmatch '^[A-Za-z]:?$') { throw 'Enter one USB drive letter, such as E.' }
    $letter = $DriveLetter.Substring(0,1).ToUpperInvariant()
    $package = Get-GuRehearsalPackage $PackageDirectory
    $required = [long]$package.Size + 1048576
    $initial = Get-GuUsbSnapshot $letter
    Assert-GuUsbSnapshot $initial $required
    $previousHash = Assert-GuPreviousRehearsalDestination $initial.Root
    Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) $required
    $null = Assert-GuPreviousRehearsalDestination $initial.Root $previousHash
    $bootDirectory = Join-Path $initial.Root 'EFI/BOOT'
    $logs = Join-Path $initial.Root 'GPU-UNLOCK-ADA'
    $image = Join-Path $bootDirectory 'BOOTX64.EFI'
    $id = [Guid]::NewGuid().ToString('N')
    $temporary = Join-Path $bootDirectory ('.gu-rehearsal-update-' + $id + '.tmp')
    $backup = Join-Path $bootDirectory ('BOOTX64-previous-' + $id + '.bak')
    try {
        Assert-GuOrdinaryPath $temporary
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($package.Bytes, 0, $package.Bytes.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if ((Get-GuBytesHash (Read-GuBoundedFile $temporary 8388608)) -cne $package.Sha256) {
            throw 'The staged rehearsal failed checksum verification.'
        }
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1048576
        $null = Assert-GuPreviousRehearsalDestination $initial.Root $previousHash
        Assert-GuOrdinaryPath $backup
        # Both moves refuse an existing destination. Reports are never opened.
        [IO.File]::Move($image, $backup)
        if ((Get-GuBytesHash (Read-GuBoundedFile $backup 8388608)) -cne $previousHash) {
            throw 'The previous EFI changed during the update; its backup has been preserved.'
        }
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1048576
        Assert-GuOrdinaryPath $image
        Assert-GuOrdinaryPath $logs
        if (-not (Test-Path -LiteralPath $logs -PathType Container)) { throw 'The report directory changed during the update.' }
        if ((Get-GuBytesHash (Read-GuBoundedFile $temporary 8388608)) -cne $package.Sha256) {
            throw 'The staged rehearsal changed before installation.'
        }
        [IO.File]::Move($temporary, $image)
        if ((Get-GuBytesHash (Read-GuBoundedFile $image 8388608)) -cne $package.Sha256) {
            throw 'The final rehearsal update failed checksum verification.'
        }
        return [pscustomobject]@{ Root=$initial.Root; Sha256=$package.Sha256; Size=$package.Size;
            BackupPath=$backup; PreviousSha256=$previousHash }
    } catch {
        throw ('USB update stopped. Do not boot this partially updated USB. Reports, backup and staged files were preserved; no automatic cleanup was attempted. Previous EFI backup, if created: ' + $backup + '. ' + $_.Exception.Message)
    }
}

Export-ModuleMember -Function Get-GuRehearsalUsbCandidates, Update-GuRehearsalUsb
