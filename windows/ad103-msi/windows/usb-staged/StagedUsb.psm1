# SPDX-License-Identifier: GPL-2.0-only
# One-board, one-release USB updater. No firmware settings or installed drivers.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$storagePath = Join-Path $PSScriptRoot 'source/windows/UsbPreparation.psm1'
if (-not (Test-Path -LiteralPath $storagePath -PathType Leaf)) {
    $storagePath = Join-Path $PSScriptRoot '../UsbPreparation.psm1'
}
$script:StorageModule = Import-Module $storagePath -Force -PassThru
$script:PreviousSha256 = 'fe02838ab242f1f785483322f8e85953df1416a42b712b3a5c75d54e495353e4'
$script:AssetPins = [ordered]@{
    'booter_load_ad102.bin' = @{bytes=57672;sha256='6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49'}
    'v67-ad102-610.43.03-four-plm.bin' = @{bytes=64000;sha256='44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb'}
    'gsp_ga10x.bin' = @{bytes=84277400;sha256='572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a'}
    'gsp_boot_ad102.bin' = @{bytes=36864;sha256='45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0'}
    'gsp_boot_desc_ad102.bin' = @{bytes=84;sha256='83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42'}
    'NVIDIA-LICENSE' = @{bytes=22653;sha256='c692fb14ad4f499c6aee1b9e144e6905148c420e6225b17cb2937366c1e507ad'}
    'NOTICE.md' = @{bytes=3663;sha256='ba51777dff822efa09e6f924cf702e7de47fe2a14101cca3573ad3db35445797'}
}
function Assert-GuOrdinaryPath([string]$Path) { & $script:StorageModule {param($p) Assert-GuOrdinaryPath $p} $Path }
function Get-GuBytesHash([byte[]]$Bytes) { & $script:StorageModule {param($b) Get-GuBytesHash $b} $Bytes }
function Read-GuBoundedFile([string]$Path,[long]$Maximum) {
    $bytes = & $script:StorageModule {param($p,$n) Read-GuBoundedFile $p $n} $Path $Maximum
    return ,$bytes
}
function Get-GuUsbSnapshot([string]$Letter) { & $script:StorageModule {param($l) Get-GuUsbSnapshot $l} $Letter }
function Assert-GuUsbSnapshot($Snapshot,[long]$Required) { & $script:StorageModule {param($s,$n) Assert-GuUsbSnapshot $s $n} $Snapshot $Required }
function Assert-GuSameUsb($Before,$After,[long]$Required) { & $script:StorageModule {param($a,$b,$n) Assert-GuSameUsb $a $b $n} $Before $After $Required }
function Get-GuStagedUsbCandidates { & $script:StorageModule {Get-GuDiagnosticUsbCandidates} }
function Test-GuInteger($Value,[long]$Minimum,[long]$Maximum) {
    return (($Value -is [int] -or $Value -is [long]) -and $Value -ge $Minimum -and $Value -le $Maximum)
}
function Assert-GuFilePin([string]$Path,$Pin) {
    Assert-GuOrdinaryPath $Path
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        if ($stream.Length -ne $Pin.bytes) { throw ('Wrong file size: ' + $Path) }
        $digest = ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-','').ToLowerInvariant()
        if ($digest -cne $Pin.sha256) { throw ('SHA-256 mismatch: ' + $Path) }
    } finally { $hash.Dispose(); $stream.Dispose() }
}
function Assert-GuAssets([string]$Path) {
    Assert-GuOrdinaryPath $Path
    $items = @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction Stop)
    if ($items.Count -ne $script:AssetPins.Count) { throw 'Expected exactly five firmware files, NVIDIA-LICENSE and NOTICE.md.' }
    foreach ($item in $items) {
        if ($item.PSIsContainer -or $item.Attributes -band [IO.FileAttributes]::ReparsePoint -or
            -not $script:AssetPins.Contains($item.Name)) { throw 'Unexpected asset directory entry.' }
    }
    foreach ($name in $script:AssetPins.Keys) { Assert-GuFilePin (Join-Path $Path $name) $script:AssetPins[$name] }
}
function Get-GuStagedPackage([string]$Directory) {
    $root = [IO.Path]::GetFullPath($Directory); Assert-GuOrdinaryPath $root
    $manifest = [Text.Encoding]::UTF8.GetString((Read-GuBoundedFile (Join-Path $root 'build-result.json') 4194304)) | ConvertFrom-Json
    foreach ($field in @('unlock_enabled','staged_unlock_enabled','assets_verified')) {
        if ($manifest.$field -isnot [bool] -or $manifest.$field -ne $true) { throw ('Missing enabled-build flag: ' + $field) }
    }
    if ($manifest.mode -cne 'staged-unlock-test' -or $manifest.version -cne '0.6.4-unlock-test' -or
        $manifest.hardware_tested -isnot [bool] -or $manifest.hardware_tested -ne $false -or
        $null -ne $manifest.helper_protocol -or $manifest.board_profile.id -ne 0x270510de -or
        $manifest.board_profile.subsystem -ne 0xe1301462L -or
        $manifest.sha256 -isnot [string] -or $manifest.sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        -not (Test-GuInteger $manifest.size 512 8388608)) { throw 'Not the complete 0.6.4 unlock-test package for this MSI 2705 board.' }
    $defines = $manifest.build_defines
    foreach ($field in @('GU_ENABLE_UNLOCK','GU_STAGED_UNLOCK','GU_REHEARSAL','GU_SESSION_HELPER')) {
        if (-not (Test-GuInteger $defines.$field 0 1)) { throw 'Malformed build definitions.' }
    }
    if ($defines.GU_ENABLE_UNLOCK -ne 1 -or $defines.GU_STAGED_UNLOCK -ne 1 -or
        $defines.GU_REHEARSAL -ne 0 -or $defines.GU_SESSION_HELPER -ne 0) { throw 'Wrong build definitions.' }
    $e = $manifest.capture_profile.staged_unlock_evidence
    if (-not (Test-GuInteger $e.schema_version 1 1) -or $e.source_version -cne '0.5.2-rehearsal' -or
        $e.restoration_verified -isnot [bool] -or $e.restoration_verified -ne $true -or
        $e.windows_boot_user_confirmed -isnot [bool] -or $e.windows_boot_user_confirmed -ne $true -or
        $e.unlock_attempted -isnot [bool] -or $e.unlock_attempted -ne $false -or
        -not (Test-GuInteger $e.app_mmio_writes 0 0) -or $e.windows_handoff -cne 'pending' -or
        $e.profile_tag -cne $manifest.board_profile.report_sha256 -or
        $e.profile_tag -cne $manifest.capture_profile.board.report_sha256 -or
        $e.profile_tag -cnotmatch '^[a-f0-9]{64}$' -or $e.report_sha256 -cnotmatch '^[a-f0-9]{64}$') {
        throw 'Successful rehearsal and separately observed Windows boot evidence are missing.'
    }
    $expected = @('app','app_io','ownership','format','runtime','sha256','checks','port','compat','probe',
                  'capture_pe','rehearsal','rehearsal_handoff','staged_state') | ForEach-Object {'windows/src/' + $_ + '.c'}
    $expected += @('unlock_610','prep_610','layout_610','vbios_610','gfw_eval') | ForEach-Object {'kernel/gpu_unlock/' + $_ + '.c'}
    $compiled = @($manifest.compiled_sources)
    if ($compiled.Count -ne $expected.Count -or @($compiled | Select-Object -Unique).Count -ne $expected.Count) { throw 'Wrong source inventory.' }
    foreach ($name in $compiled) { if ($name -isnot [string] -or $expected -cnotcontains $name) { throw 'Unexpected compiled source.' } }
    $required = @($manifest.required_assets.PSObject.Properties)
    if ($required.Count -ne 5) { throw 'Incomplete firmware manifest.' }
    foreach ($asset in $required) {
        if ($asset.Name -notlike '*.bin' -or -not $script:AssetPins.Contains($asset.Name)) { throw 'Unknown required firmware.' }
        $pin = $script:AssetPins[$asset.Name]
        if (-not (Test-GuInteger $asset.Value.bytes $pin.bytes $pin.bytes) -or $asset.Value.sha256 -cne $pin.sha256) { throw 'Wrong firmware pin.' }
    }
    $image = Join-Path $root 'BOOTX64.EFI'; $bytes = Read-GuBoundedFile $image 8388608
    if ($bytes.Length -ne $manifest.size -or (Get-GuBytesHash $bytes) -cne $manifest.sha256) { throw 'EFI hash/size mismatch; extract the original ZIP again.' }
    $pe = [BitConverter]::ToUInt32($bytes,60)
    if ($bytes[0] -ne 77 -or $bytes[1] -ne 90 -or $pe -gt ($bytes.Length-94) -or
        [BitConverter]::ToUInt32($bytes,$pe) -ne 0x00004550 -or [BitConverter]::ToUInt16($bytes,$pe+4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes,$pe+24) -ne 0x20b -or [BitConverter]::ToUInt16($bytes,$pe+92) -ne 10) { throw 'Not an x64 UEFI application.' }
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    foreach ($marker in @('gpu-unlock-ada Windows preboot port 0.6.4-unlock-test','mode=staged-unlock-test',
                         'STAGED_UNLOCK_INTENT:','UNLOCK_VERIFIED:','Falcon start','booter_load_ad102.bin')) {
        if (-not $text.Contains($marker)) { throw ('Missing actual-unlock marker: ' + $marker) }
    }
    return [pscustomobject]@{Root=$root;Bytes=$bytes;Sha256=$manifest.sha256;Size=$bytes.Length}
}
function Assert-GuPrevious([string]$Root,[string]$Expected='') {
    foreach ($relative in @('','EFI','EFI/BOOT','GPU-UNLOCK-ADA')) {
        $path = if ($relative) { Join-Path $Root $relative } else { $Root }
        Assert-GuOrdinaryPath $path
        if (-not (Get-Item -LiteralPath $path -Force -ErrorAction Stop).PSIsContainer) { throw 'Expected existing ordinary USB directories.' }
    }
    $hash = Get-GuBytesHash (Read-GuBoundedFile (Join-Path $Root 'EFI/BOOT/BOOTX64.EFI') 8388608)
    if ($hash -cne $script:PreviousSha256 -or ($Expected -and $hash -cne $Expected)) {
        throw 'Only the exact released 0.6.3 unlock-test USB can be updated. An unknown or already-updated boot image will not be replaced.'
    }
    return $hash
}
function Assert-GuAbsent([string]$Path) {
    Assert-GuOrdinaryPath $Path
    if ($null -ne (Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue)) { throw ('Refusing an existing destination: ' + $Path) }
}
function Update-GuStagedUsb {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$PackageDirectory,[Parameter(Mandatory=$true)][string]$DriveLetter)
    $DriveLetter = $DriveLetter.Trim()
    if ($DriveLetter -notmatch '^[A-Za-z]:?$') { throw 'Enter one USB drive letter, such as E.' }
    $letter = $DriveLetter.Substring(0,1).ToUpperInvariant()
    $package = Get-GuStagedPackage $PackageDirectory
    $required = [long]$package.Size + 1MB
    $initial = Get-GuUsbSnapshot $letter; Assert-GuUsbSnapshot $initial $required
    $previous = Assert-GuPrevious $initial.Root
    $assets = Join-Path $initial.Root 'GPU-UNLOCK-ADA/ASSETS'
    # Upgrade only: inspect all seven existing files in place. Never download,
    # regenerate, copy, rename, replace or delete the firmware asset directory.
    Assert-GuAssets $assets
    Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) $required
    $null = Assert-GuPrevious $initial.Root $previous; Assert-GuAssets $assets
    $id = [Guid]::NewGuid().ToString('N')
    $boot = Join-Path $initial.Root 'EFI/BOOT'; $image = Join-Path $boot 'BOOTX64.EFI'
    $stagedImage = Join-Path $boot ('.unlock-image-' + $id + '.tmp')
    $backup = Join-Path $boot ('BOOTX64-0.6.3-' + $id + '.bak')
    try {
        Assert-GuAbsent $stagedImage
        $stream = [IO.File]::Open($stagedImage,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $stream.Write($package.Bytes,0,$package.Bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
        $imagePin = @{bytes=$package.Size;sha256=$package.Sha256}; Assert-GuFilePin $stagedImage $imagePin
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1MB
        $null = Assert-GuPrevious $initial.Root $previous
        Assert-GuAssets $assets; Assert-GuFilePin $stagedImage $imagePin; Assert-GuAbsent $backup
        [IO.File]::Move($image,$backup)
        if ((Get-GuBytesHash (Read-GuBoundedFile $backup 8388608)) -cne $previous) { throw 'The previous EFI changed; its backup was preserved.' }
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1MB
        Assert-GuAbsent $image; Assert-GuFilePin $stagedImage $imagePin; Assert-GuAssets $assets
        [IO.File]::Move($stagedImage,$image); Assert-GuFilePin $image $imagePin
        Assert-GuSameUsb $initial (Get-GuUsbSnapshot $letter) 1MB; Assert-GuAssets $assets
        return [pscustomobject]@{Root=$initial.Root;Sha256=$package.Sha256;BackupPath=$backup;AssetsPath=$assets;PreviousSha256=$previous}
    } catch {
        throw ('Update stopped; do not boot a partially updated USB. Existing assets, reports, staged files and any backup were preserved. No automatic cleanup was attempted. Previous-EFI backup path: ' + $backup + '. ' + $_.Exception.Message)
    }
}

Export-ModuleMember -Function Get-GuStagedUsbCandidates,Update-GuStagedUsb
