# SPDX-License-Identifier: GPL-2.0-only
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
# Only the exact EFIs from the released 0.4 and 0.4.1 diagnostics may be updated.
# This is an accidental-overwrite guard, not an authenticity boundary against an
# administrator editing the module itself. No public command accepts a bypass.
$script:ApprovedPreviousDiagnosticSha256 = @(
    '799442355c1de89d704263556c091d4becedad61273c4ce1ba022fd2aadfbac1',
    'ba11eb474ee21e5daba2b20018534df24af7544a0b64e5b717f8b54f98acbe2e'
)

function Assert-GuOrdinaryPath {
    param([Parameter(Mandatory=$true)][string]$Path)
    # Check existing ancestors as well as the leaf, including dangling links.
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw ('Refusing a symbolic link or reparse point: ' + $current)
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) { break }
        $current = $parent.FullName
    }
}

function Get-GuBytesHash {
    param([byte[]]$Bytes)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose() }
}

function Read-GuBoundedFile {
    param([string]$Path, [long]$MaximumBytes)
    Assert-GuOrdinaryPath $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -gt $MaximumBytes) { throw ('File is larger than allowed: ' + $Path) }
        $bytes = New-Object byte[] ([int]$stream.Length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $read = $stream.Read($bytes, $offset, $bytes.Length - $offset)
            if ($read -le 0) { throw ('Incomplete file read: ' + $Path) }
            $offset += $read
        }
        return ,$bytes
    } finally { $stream.Dispose() }
}

function Get-GuDiagnosticPackage {
    param([string]$PackageDirectory)
    $root = [IO.Path]::GetFullPath($PackageDirectory)
    Assert-GuOrdinaryPath $root
    $usb = Join-Path $root 'USB'
    $image = Join-Path (Join-Path (Join-Path $usb 'EFI') 'BOOT') 'BOOTX64.EFI'
    Assert-GuOrdinaryPath $image
    # No additional boot payloads, assets or files may be copied from USB/.
    $allItems = @(Get-ChildItem -LiteralPath $usb -Force -Recurse -ErrorAction Stop)
    foreach ($item in $allItems) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'USB payload contains a symbolic link or reparse point.'
        }
    }
    $files = @($allItems | Where-Object { -not $_.PSIsContainer })
    if ($files.Count -ne 1 -or $files[0].FullName -cne $image) {
        throw 'USB payload must contain only EFI/BOOT/BOOTX64.EFI.'
    }
    $manifestBytes = Read-GuBoundedFile (Join-Path $root 'build-result.json') 1048576
    $manifest = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
    if ($manifest.mode -cne 'diagnostic' -or $manifest.hardware_tested -isnot [bool] -or
        $manifest.hardware_tested -ne $false -or $null -ne $manifest.board_profile -or
        $null -ne $manifest.helper_protocol -or $manifest.sha256 -isnot [string] -or
        $manifest.sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        ($manifest.size -isnot [int] -and $manifest.size -isnot [long]) -or
        $manifest.size -lt 512 -or $manifest.size -gt 8388608) {
        throw 'Not a complete read-only diagnostic build manifest.'
    }
    $bytes = Read-GuBoundedFile $image 8388608
    if ($bytes.Length -ne $manifest.size -or (Get-GuBytesHash $bytes) -cne $manifest.sha256) {
        throw 'Diagnostic EFI checksum or size mismatch. Extract the original ZIP again.'
    }
    # Independently reject non-AMD64 / non-UEFI images even if their hash matches.
    $pe = [BitConverter]::ToUInt32($bytes, 60)
    if ($bytes[0] -ne 77 -or $bytes[1] -ne 90 -or $pe -gt ($bytes.Length - 94) -or
        [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b -or
        [BitConverter]::ToUInt16($bytes, $pe + 92) -ne 10) {
        throw 'Diagnostic payload is not an x64 UEFI application.'
    }
    return [pscustomobject]@{ Bytes=$bytes; Sha256=$manifest.sha256; Size=$bytes.Length }
}

function Get-GuUsbSnapshot {
    param([string]$DriveLetter)
    $partitions = @(Get-Partition -DriveLetter $DriveLetter -ErrorAction Stop)
    $volumes = @(Get-Volume -DriveLetter $DriveLetter -ErrorAction Stop)
    if ($partitions.Count -ne 1 -or $volumes.Count -ne 1) { throw 'The USB volume could not be identified uniquely.' }
    $partition = $partitions[0]
    $disks = @(Get-Disk -Number $partition.DiskNumber -ErrorAction Stop)
    if ($disks.Count -ne 1) { throw 'The USB disk could not be identified uniquely.' }
    $disk = $disks[0]; $volume = $volumes[0]
    return [pscustomobject]@{
        DriveLetter=$DriveLetter; Root=($DriveLetter + ':\'); Label=[string]$volume.FileSystemLabel
        DiskNumber=$disk.Number; DiskUniqueId=[string]$disk.UniqueId; DiskPath=[string]$disk.Path
        PartitionNumber=$partition.PartitionNumber; PartitionOffset=$partition.Offset
        VolumeUniqueId=[string]$volume.UniqueId; VolumeSize=$volume.Size
        BusType=[string]$disk.BusType; FileSystem=[string]$volume.FileSystem
        DiskIsBoot=$disk.IsBoot; DiskIsSystem=$disk.IsSystem; DiskIsReadOnly=$disk.IsReadOnly
        DiskIsOffline=$disk.IsOffline; PartitionIsBoot=$partition.IsBoot; PartitionIsSystem=$partition.IsSystem
        FreeBytes=$volume.SizeRemaining; SystemDrive=[string]$env:SystemDrive
    }
}

function Assert-GuUsbSnapshot {
    param($Snapshot, [long]$RequiredBytes)
    foreach ($field in @('DiskIsBoot','DiskIsSystem','DiskIsReadOnly','DiskIsOffline','PartitionIsBoot','PartitionIsSystem')) {
        if ($Snapshot.$field -isnot [bool] -or $Snapshot.$field) {
            throw 'Refusing a boot, system, read-only, offline, or incompletely identified disk.'
        }
    }
    if ($Snapshot.BusType -cne 'USB' -or $Snapshot.FileSystem -cne 'FAT32' -or
        ($Snapshot.DriveLetter + ':') -ieq $Snapshot.SystemDrive) {
        throw 'Destination must be an existing FAT32 USB volume outside the Windows system disk. No formatting was performed.'
    }
    if ([string]::IsNullOrWhiteSpace($Snapshot.VolumeUniqueId) -or
        ([string]::IsNullOrWhiteSpace($Snapshot.DiskUniqueId) -and [string]::IsNullOrWhiteSpace($Snapshot.DiskPath))) {
        throw 'Windows did not provide enough USB identity information to copy safely.'
    }
    if ($Snapshot.FreeBytes -lt $RequiredBytes) { throw 'The USB needs room for the diagnostic plus 1 MiB for reports.' }
}

function Assert-GuSameUsb {
    param($Before, $After, [long]$RequiredBytes)
    Assert-GuUsbSnapshot $After $RequiredBytes
    foreach ($field in @('DriveLetter','Root','DiskNumber','DiskUniqueId','DiskPath','PartitionNumber',
                         'PartitionOffset','VolumeUniqueId','VolumeSize')) {
        if ([string]$Before.$field -cne [string]$After.$field) {
            throw 'The USB identity changed while preparing it. No further files will be copied.'
        }
    }
}

function Assert-GuEmptyDiagnosticDestination {
    param([string]$Root)
    foreach ($path in @($Root, (Join-Path $Root 'EFI'), (Join-Path (Join-Path $Root 'EFI') 'BOOT'))) {
        Assert-GuOrdinaryPath $path
        $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and -not $item.PSIsContainer) { throw ('A directory is occupied by a file: ' + $path) }
    }
    foreach ($path in @((Join-Path (Join-Path (Join-Path $Root 'EFI') 'BOOT') 'BOOTX64.EFI'),
                        (Join-Path $Root 'GPU-UNLOCK-ADA'))) {
        Assert-GuOrdinaryPath $path
        if ($null -ne (Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue)) {
            throw 'Refusing to overwrite an existing bootloader or GPU-UNLOCK-ADA directory. Use a different USB.'
        }
    }
}

function Get-GuDiagnosticUsbCandidates {
    foreach ($volume in @(Get-Volume -ErrorAction Stop | Where-Object { $_.DriveLetter })) {
        try {
            $snapshot = Get-GuUsbSnapshot ([string]$volume.DriveLetter)
            if ($snapshot.BusType -ceq 'USB') { $snapshot }
        } catch { }
    }
}

function New-GuDiagnosticUsb {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$PackageDirectory,
          [Parameter(Mandatory=$true)][string]$DriveLetter)
    $DriveLetter = $DriveLetter.Trim()
    if ($DriveLetter -notmatch '^[A-Za-z]:?$') { throw 'Enter one USB drive letter, such as E.' }
    $letter = $DriveLetter.Substring(0,1).ToUpperInvariant()
    $package = Get-GuDiagnosticPackage $PackageDirectory
    $required = [long]$package.Size + 1048576
    $initial = Get-GuUsbSnapshot $letter
    Assert-GuUsbSnapshot $initial $required
    Assert-GuEmptyDiagnosticDestination $initial.Root
    # Refresh immediately before making any directories or files.
    Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) $required
    Assert-GuEmptyDiagnosticDestination $initial.Root
    $efi = Join-Path $initial.Root 'EFI'
    $bootDirectory = Join-Path $efi 'BOOT'
    $logs = Join-Path $initial.Root 'GPU-UNLOCK-ADA'
    $image = Join-Path $bootDirectory 'BOOTX64.EFI'
    $temporary = Join-Path $bootDirectory ('.gu-diagnostic-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $started = $false
    try {
        $started = $true
        [IO.Directory]::CreateDirectory($bootDirectory) | Out-Null
        Assert-GuEmptyDiagnosticDestination $initial.Root
        # New-Item refuses an existing directory; do not use -Force for the report root.
        New-Item -ItemType Directory -Path $logs -ErrorAction Stop | Out-Null
        Assert-GuOrdinaryPath $logs
        Assert-GuOrdinaryPath $temporary
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($package.Bytes, 0, $package.Bytes.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if ((Get-GuBytesHash (Read-GuBoundedFile $temporary 8388608)) -cne $package.Sha256) {
            throw 'The USB temporary copy failed checksum verification.'
        }
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1048576
        Assert-GuOrdinaryPath $temporary
        Assert-GuOrdinaryPath $image
        Assert-GuOrdinaryPath $logs
        # File.Move's two-argument overload fails if the destination already exists.
        [IO.File]::Move($temporary, $image)
        if ((Get-GuBytesHash (Read-GuBoundedFile $image 8388608)) -cne $package.Sha256) {
            throw 'The final USB copy failed checksum verification.'
        }
        return [pscustomobject]@{ Root=$initial.Root; Sha256=$package.Sha256; Size=$package.Size }
    } catch {
        if ($started) {
            throw ('Preparation stopped after creating USB files. Do not boot this USB. No automatic cleanup was attempted; inspect EFI/BOOT and GPU-UNLOCK-ADA. ' + $_.Exception.Message)
        }
        throw
    }
}

function Assert-GuExistingDiagnosticDestination {
    param([string]$Root, [string]$ExpectedSha256)
    foreach ($path in @($Root, (Join-Path $Root 'EFI'), (Join-Path (Join-Path $Root 'EFI') 'BOOT'),
                        (Join-Path $Root 'GPU-UNLOCK-ADA'))) {
        Assert-GuOrdinaryPath $path
        $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
        if (-not $item.PSIsContainer) { throw ('Expected an existing ordinary directory: ' + $path) }
    }
    $image = Join-Path (Join-Path (Join-Path $Root 'EFI') 'BOOT') 'BOOTX64.EFI'
    $previousHash = Get-GuBytesHash (Read-GuBoundedFile $image 8388608)
    if ($script:ApprovedPreviousDiagnosticSha256 -cnotcontains $previousHash) {
        throw 'This USB does not contain an exact released 0.4 or 0.4.1 diagnostic. Its existing EFI was not changed. Use Prepare-USB.cmd with a different USB.'
    }
    if ($ExpectedSha256 -and $previousHash -cne $ExpectedSha256) {
        throw 'The previous diagnostic EFI changed during the update. No further files will be copied.'
    }
    return $previousHash
}

function Update-GuDiagnosticUsb {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$PackageDirectory,
          [Parameter(Mandatory=$true)][string]$DriveLetter)
    $DriveLetter = $DriveLetter.Trim()
    if ($DriveLetter -notmatch '^[A-Za-z]:?$') { throw 'Enter one USB drive letter, such as E.' }
    $letter = $DriveLetter.Substring(0,1).ToUpperInvariant()
    $package = Get-GuDiagnosticPackage $PackageDirectory
    $required = [long]$package.Size + 1048576
    $initial = Get-GuUsbSnapshot $letter
    Assert-GuUsbSnapshot $initial $required
    $previousHash = Assert-GuExistingDiagnosticDestination $initial.Root
    Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) $required
    $null = Assert-GuExistingDiagnosticDestination $initial.Root $previousHash

    $bootDirectory = Join-Path (Join-Path $initial.Root 'EFI') 'BOOT'
    $logs = Join-Path $initial.Root 'GPU-UNLOCK-ADA'
    $image = Join-Path $bootDirectory 'BOOTX64.EFI'
    $id = [Guid]::NewGuid().ToString('N')
    $temporary = Join-Path $bootDirectory ('.gu-diagnostic-update-' + $id + '.tmp')
    $backup = Join-Path $bootDirectory ('BOOTX64-previous-' + $id + '.bak')
    try {
        Assert-GuOrdinaryPath $temporary
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($package.Bytes, 0, $package.Bytes.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if ((Get-GuBytesHash (Read-GuBoundedFile $temporary 8388608)) -cne $package.Sha256) {
            throw 'The staged diagnostic update failed checksum verification.'
        }
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1048576
        $null = Assert-GuExistingDiagnosticDestination $initial.Root $previousHash
        Assert-GuOrdinaryPath $backup
        # Neither move overwrites a destination. The original image is retained,
        # and no files inside the report directory are opened or changed.
        [IO.File]::Move($image, $backup)
        if ((Get-GuBytesHash (Read-GuBoundedFile $backup 8388608)) -cne $previousHash) {
            throw 'The previous EFI changed during the update; its backup has been preserved.'
        }
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1048576
        Assert-GuOrdinaryPath $image
        Assert-GuOrdinaryPath $logs
        if (-not (Test-Path -LiteralPath $logs -PathType Container)) { throw 'The report directory changed during the update.' }
        if ((Get-GuBytesHash (Read-GuBoundedFile $temporary 8388608)) -cne $package.Sha256) {
            throw 'The staged diagnostic changed before installation.'
        }
        [IO.File]::Move($temporary, $image)
        if ((Get-GuBytesHash (Read-GuBoundedFile $image 8388608)) -cne $package.Sha256) {
            throw 'The final diagnostic update failed checksum verification.'
        }
        return [pscustomobject]@{ Root=$initial.Root; Sha256=$package.Sha256; Size=$package.Size; BackupPath=$backup; PreviousSha256=$previousHash }
    } catch {
        throw ('USB update stopped. Do not boot this partially updated USB. Existing reports and any backup or staged files were preserved; no automatic cleanup was attempted. Previous EFI backup, if the move completed: ' + $backup + '. ' + $_.Exception.Message)
    }
}

Export-ModuleMember -Function Get-GuDiagnosticUsbCandidates, New-GuDiagnosticUsb, Update-GuDiagnosticUsb
