# SPDX-License-Identifier: GPL-2.0-only
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:BackendRoot = $PSScriptRoot

function Get-GuBytesHash {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Assert-GuRelativePath {
    param([string]$Path)
    if (-not $Path -or $Path.Length -gt 180 -or $Path -notmatch '\A[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*(?:/[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*)*\z') {
        throw 'Bundle paths must be simple relative paths with forward slashes.'
    }
    foreach ($part in $Path.Split('/')) {
        if ($part -match '\A(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|\z)') { throw 'Reserved Windows file name.' }
    }
}

function Assert-GuEsp {
    param($Esp)
    if (-not $Esp -or [uint64]$Esp.Size -lt 1048576 -or [uint64]$Esp.Offset -eq 0 -or
        [int]$Esp.PartitionNumber -lt 1 -or [int]$Esp.DiskNumber -lt 0 -or
        [uint32]$Esp.LogicalSectorSize -notin @(512, 4096) -or
        [uint64]$Esp.Offset % [uint32]$Esp.LogicalSectorSize -ne 0 -or
        [uint64]$Esp.Size % [uint32]$Esp.LogicalSectorSize -ne 0 -or
        [guid]$Esp.PartitionGuid -eq [guid]::Empty) { throw 'Incomplete or unsupported EFI system partition geometry.' }
}

function Get-GuSystemEsp {
    # Storage-provider inventory only: does not mount or write the ESP.
    $matches = @(Get-Partition -ErrorAction Stop | Where-Object {
        $_.IsSystem -and ([string]$_.GptType).Trim('{}') -ieq 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
    })
    if ($matches.Count -ne 1) { throw 'A single GPT system EFI partition is required.' }
    $p = $matches[0]; $d = Get-Disk -Number $p.DiskNumber -ErrorAction Stop
    if ($d.PartitionStyle -ne 'GPT' -or $d.IsOffline -or $d.IsReadOnly) { throw 'The system disk is not an online writable GPT disk.' }
    $esp = [pscustomobject]@{
        DiskNumber = [int]$p.DiskNumber; PartitionNumber = [int]$p.PartitionNumber
        PartitionGuid = ([guid]$p.Guid).ToString('D'); Offset = [uint64]$p.Offset
        Size = [uint64]$p.Size; LogicalSectorSize = [uint32]$d.LogicalSectorSize
    }
    Assert-GuEsp $esp
    return $esp
}

function New-GuBootPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Snapshot, [Parameter(Mandatory)]$Bundle)
    if ($Bundle.Validated -isnot [bool] -or $Bundle.Validated -ne $true -or $Bundle.HelperProtocol -cne 'gu-session-v1') { throw 'A reviewed session-helper bundle is required.' }
    if ($Bundle.ManifestSha256 -cnotmatch '\A[0-9a-f]{64}\z' -or $Bundle.ProfileSha256 -cnotmatch '\A[0-9a-f]{64}\z' -or -not $Bundle.Id) { throw 'Missing reviewed bundle identity.' }
    Assert-GuEsp $Snapshot.Esp
    $files = @(); $names = @{}
    foreach ($f in @($Bundle.Files)) {
        Assert-GuRelativePath $f.RelativePath
        if ($f.RelativePath -ieq 'REQUEST.BIN' -or $names.ContainsKey($f.RelativePath) -or $f.Sha256 -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Duplicate bundle path or invalid SHA-256.' }
        $names[$f.RelativePath] = $true
        $files += [pscustomobject]@{ SourcePath = [IO.Path]::GetFullPath($f.SourcePath); RelativePath = [string]$f.RelativePath; Sha256 = [string]$f.Sha256 }
    }
    Assert-GuRelativePath $Bundle.EfiRelativePath
    if ($files.Count -lt 1 -or $files.Count -gt 32 -or -not $names.ContainsKey($Bundle.EfiRelativePath) -or $Bundle.EfiRelativePath -cne 'BOOTX64.EFI') { throw 'Bundle has no declared EFI application.' }
    return [pscustomobject]@{
        SchemaVersion = 1; SessionId = [guid]::NewGuid().ToString('N')
        Esp = $Snapshot.Esp; Files = $files; EfiRelativePath = [string]$Bundle.EfiRelativePath
        BundleId = [string]$Bundle.Id; BundleSha256 = [string]$Bundle.ManifestSha256; ProfileSha256 = [string]$Bundle.ProfileSha256
        HelperProtocol = 'gu-session-v1'; Validated = $true
    }
}

function New-GuEfiLoadOption {
    param([Parameter(Mandatory)]$Plan, [byte[]]$OptionalData = @())
    Assert-GuEsp $Plan.Esp
    if ($Plan.SessionId -cnotmatch '\A[0-9a-f]{32}\z') { throw 'Invalid session ID.' }
    Assert-GuRelativePath $Plan.EfiRelativePath
    $path = '\EFI\GpuUnlockSession\' + $Plan.SessionId + '\' + $Plan.EfiRelativePath.Replace('/', '\')
    $pathBytes = [Text.Encoding]::Unicode.GetBytes($path + [char]0)
    $description = [Text.Encoding]::Unicode.GetBytes('GPU Unlock session ' + $Plan.SessionId + [char]0)
    $stream = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $writer.Write([uint32]1) # LOAD_OPTION_ACTIVE
        $writer.Write([uint16](42 + 4 + $pathBytes.Length + 4))
        $writer.Write($description)
        $writer.Write([byte]4); $writer.Write([byte]1); $writer.Write([uint16]42) # MEDIA_HARDDRIVE_DP
        $writer.Write([uint32]$Plan.Esp.PartitionNumber)
        $writer.Write([uint64]([uint64]$Plan.Esp.Offset / [uint32]$Plan.Esp.LogicalSectorSize))
        $writer.Write([uint64]([uint64]$Plan.Esp.Size / [uint32]$Plan.Esp.LogicalSectorSize))
        $writer.Write(([guid]$Plan.Esp.PartitionGuid).ToByteArray())
        $writer.Write([byte]2); $writer.Write([byte]2) # GPT, GUID signature
        $writer.Write([byte]4); $writer.Write([byte]4); $writer.Write([uint16](4 + $pathBytes.Length))
        $writer.Write($pathBytes)
        $writer.Write([byte]0x7f); $writer.Write([byte]0xff); $writer.Write([uint16]4)
        $writer.Write($OptionalData)
        return ,$stream.ToArray()
    } finally { $writer.Dispose(); $stream.Dispose() }
}

function Test-GuVariableEqual {
    param($Value, [byte[]]$Bytes)
    return ($null -ne $Value -and [uint32]$Value.Attributes -eq 7 -and
        [Convert]::ToBase64String([byte[]]$Value.Bytes) -ceq [Convert]::ToBase64String($Bytes))
}

function Assert-GuPlan {
    param($Plan)
    if ($Plan.SchemaVersion -ne 1 -or $Plan.Validated -isnot [bool] -or $Plan.Validated -ne $true -or $Plan.HelperProtocol -cne 'gu-session-v1' -or
        $Plan.SessionId -cnotmatch '\A[0-9a-f]{32}\z' -or $Plan.BundleSha256 -cnotmatch '\A[0-9a-f]{64}\z' -or $Plan.ProfileSha256 -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Invalid or unreviewed boot plan.' }
    Assert-GuEsp $Plan.Esp
    Assert-GuRelativePath $Plan.EfiRelativePath
    $seen = @{}
    foreach ($f in @($Plan.Files)) {
        Assert-GuRelativePath $f.RelativePath
        if ($f.RelativePath -ieq 'REQUEST.BIN' -or $seen.ContainsKey($f.RelativePath) -or $f.Sha256 -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Invalid plan files.' }
        $seen[$f.RelativePath] = $true
    }
    if ($seen.Count -lt 1 -or $seen.Count -gt 32 -or $Plan.EfiRelativePath -cne 'BOOTX64.EFI' -or -not $seen.ContainsKey($Plan.EfiRelativePath)) { throw 'Missing EFI payload.' }
}

function Invoke-GuBootStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Plan, [Parameter(Mandatory)][scriptblock]$AuthorizePlan, [hashtable]$Adapter)
    Assert-GuPlan $Plan
    # Policy is checked before privilege activation, mount points, or any journal creation.
    $authorization = & $AuthorizePlan $Plan
    if ($authorization -isnot [bool] -or -not $authorization) { throw 'Policy did not authorize this exact boot plan.' }
    if (-not $Adapter) { $Adapter = New-GuNativeBootAdapter }
    $lock = $null; $mount = $null; $receipt = $null
    try {
        $lock = & $Adapter.Lock
        if ($null -ne (& $Adapter.ReadReceipt)) { throw 'A previous session receipt exists. Inspect or cancel it first.' }
        if ($null -ne (& $Adapter.ReadVariable 'BootNext')) { throw 'Another one-time boot is already scheduled. It has not been changed.' }
        $originalOrder = & $Adapter.ReadVariable 'BootOrder'
        if ($null -eq $originalOrder -or ([byte[]]$originalOrder.Bytes).Length -lt 2 -or ([byte[]]$originalOrder.Bytes).Length % 2) { throw 'A well-formed normal BootOrder is required.' }
        $referenced = @{}
        $orderBytes = [byte[]]$originalOrder.Bytes
        for ($i = 0; $i -lt $orderBytes.Length; $i += 2) { $referenced[[int][BitConverter]::ToUInt16($orderBytes, $i)] = $true }
        $current = & $Adapter.ReadVariable 'BootCurrent'
        if ($null -ne $current) {
            if (([byte[]]$current.Bytes).Length -ne 2) { throw 'Malformed BootCurrent.' }
            $referenced[[int][BitConverter]::ToUInt16([byte[]]$current.Bytes, 0)] = $true
        }
        $slot = -1
        # Reserve from a bounded range; never recycle an existing firmware entry.
        for ($id = 0x7000; $id -lt 0x7100; $id++) {
            $name = 'Boot{0:X4}' -f $id
            if (-not $referenced.ContainsKey($id) -and $null -eq (& $Adapter.ReadVariable $name)) { $slot = $id; break }
        }
        if ($slot -lt 0) { throw 'No unused session boot-option slot is available.' }
        $optionName = 'Boot{0:X4}' -f $slot
        # OptionalData is bound to the same per-attempt ID. The EFI helper validates this protocol.
        $optional = [Text.Encoding]::ASCII.GetBytes('GUSSN001' + $Plan.SessionId + $Plan.ProfileSha256)
        $request = [Text.Encoding]::ASCII.GetBytes('GUARM001' + $Plan.SessionId + $Plan.ProfileSha256)
        $consumed = [Text.Encoding]::ASCII.GetBytes('GUSED001' + $Plan.SessionId + $Plan.ProfileSha256)
        $option = New-GuEfiLoadOption -Plan $Plan -OptionalData $optional
        $next = [BitConverter]::GetBytes([uint16]$slot)
        $receipt = [pscustomobject]@{
            SchemaVersion = 1; SessionId = $Plan.SessionId; BundleId = $Plan.BundleId
            BundleSha256 = $Plan.BundleSha256; ProfileSha256 = $Plan.ProfileSha256; Esp = $Plan.Esp; State = 'Preparing'; LastError = ''
            CreatedUtc = [DateTime]::UtcNow.ToString('o'); BootName = $optionName
            BootBytesBase64 = [Convert]::ToBase64String($option); NextBytesBase64 = [Convert]::ToBase64String($next)
            OriginalBootOrderBase64 = $(if ($null -eq $originalOrder) { $null } else { [Convert]::ToBase64String([byte[]]$originalOrder.Bytes) })
            Files = @($Plan.Files | ForEach-Object { [pscustomobject]@{ RelativePath = $_.RelativePath; Sha256 = $_.Sha256; AlternateSha256 = '' } }) +
                @([pscustomobject]@{ RelativePath = 'REQUEST.BIN'; Sha256 = (Get-GuBytesHash $request); AlternateSha256 = (Get-GuBytesHash $consumed) })
        }
        & $Adapter.WriteReceipt $receipt # Intent is durable BEFORE any ESP/firmware mutation.
        [uint64]$payloadBytes = 104
        foreach ($f in $Plan.Files) {
            $length = (Get-Item -LiteralPath $f.SourcePath).Length
            if ($length -lt 1 -or $length -gt 134217728) { throw 'Payload size is outside the supported bound.' }
            $payloadBytes += [uint64]$length
        }
        if ($payloadBytes -gt 167772160) { throw 'Bundle exceeds the 160 MiB session limit.' }
        $mount = & $Adapter.MountEsp $Plan.Esp
        if ([uint64]$mount.FreeBytes -lt $payloadBytes + 8388608) { throw 'The EFI partition lacks space for the bundle and an 8 MiB log margin. No partition changes were made.' }
        & $Adapter.CreateSessionDirectory $mount $Plan.SessionId
        foreach ($f in $Plan.Files) {
            if ((Get-Item -LiteralPath $f.SourcePath).Length -gt 134217728) { throw 'Payload is too large.' }
            $bytes = [IO.File]::ReadAllBytes($f.SourcePath)
            if ($bytes.Length -gt 134217728 -or (Get-GuBytesHash $bytes) -cne $f.Sha256) { throw ('Payload changed: ' + $f.RelativePath) }
            & $Adapter.PutFile $mount $Plan.SessionId $f.RelativePath $bytes
            if ((& $Adapter.GetFileHash $mount $Plan.SessionId $f.RelativePath) -cne $f.Sha256) { throw 'Staged file readback failed.' }
        }
        & $Adapter.PutFile $mount $Plan.SessionId 'REQUEST.BIN' $request
        if ((& $Adapter.GetFileHash $mount $Plan.SessionId 'REQUEST.BIN') -cne (Get-GuBytesHash $request)) { throw 'Request readback failed.' }
        # Recheck after staging, before firmware mutation. The OS provides no firmware compare-and-swap.
        if ($null -ne (& $Adapter.ReadVariable 'BootNext') -or $null -ne (& $Adapter.ReadVariable $optionName)) { throw 'Boot variables changed during preparation.' }
        & $Adapter.WriteVariable $optionName $option
        if (-not (Test-GuVariableEqual (& $Adapter.ReadVariable $optionName) $option)) { throw 'Boot option readback failed.' }
        $receipt.State = 'EntryCreated'; & $Adapter.WriteReceipt $receipt
        if ($null -ne (& $Adapter.ReadVariable 'BootNext')) { throw 'Another tool scheduled a boot during preparation.' }
        & $Adapter.WriteVariable 'BootNext' $next
        if (-not (Test-GuVariableEqual (& $Adapter.ReadVariable 'BootNext') $next)) { throw 'BootNext readback failed.' }
        $newOrder = & $Adapter.ReadVariable 'BootOrder'
        $newOrderBase64 = if ($null -eq $newOrder) { $null } else { [Convert]::ToBase64String([byte[]]$newOrder.Bytes) }
        if ($newOrderBase64 -cne $receipt.OriginalBootOrderBase64) { throw 'BootOrder changed externally; automatic activation stopped.' }
        $receipt.State = 'Scheduled'; & $Adapter.WriteReceipt $receipt
        return $receipt
    } catch {
        $originalError = $_
        if ($null -ne $receipt) {
            $receipt.State = 'RecoveryNeeded'; $receipt.LastError = $originalError.Exception.Message
            try { & $Adapter.WriteReceipt $receipt } catch { }
            # Rollback uses the durable ownership record; it never restores arbitrary old NVRAM values.
            try { $null = Undo-GuBootStage -Receipt $receipt -Adapter $Adapter -AlreadyLocked -MountedEsp $mount } catch { }
        }
        throw $originalError
    } finally {
        try { if ($null -ne $mount) { & $Adapter.UnmountEsp $mount } }
        finally { if ($null -ne $lock) { & $Adapter.Unlock $lock } }
    }
}

function Assert-GuReceipt {
    param($Receipt)
    if ($Receipt.SchemaVersion -ne 1 -or $Receipt.SessionId -cnotmatch '\A[0-9a-f]{32}\z' -or
        $Receipt.BootName -cnotmatch '\ABoot70[0-9A-F]{2}\z' -or $Receipt.ProfileSha256 -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Invalid session receipt.' }
    Assert-GuEsp $Receipt.Esp
    $option = [Convert]::FromBase64String($Receipt.BootBytesBase64)
    $next = [Convert]::FromBase64String($Receipt.NextBytesBase64)
    if ($option.Length -gt 65535 -or $next.Length -ne 2 -or
        ('Boot{0:X4}' -f [BitConverter]::ToUInt16($next, 0)) -cne $Receipt.BootName) { throw 'Receipt boot identity mismatch.' }
    # Validate that recorded bytes are the exact format this backend creates, including the nonce.
    $files = @($Receipt.Files); $seen = @{}
    foreach ($f in $files) {
        Assert-GuRelativePath $f.RelativePath
        if ($seen.ContainsKey($f.RelativePath) -or $f.Sha256 -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Invalid receipt files.' }
        $seen[$f.RelativePath] = $true
    }
    if ($files.Count -lt 2 -or $files.Count -gt 33 -or -not $seen.ContainsKey('REQUEST.BIN')) { throw 'Invalid receipt file count.' }
    foreach ($f in $files) {
        if ($f.RelativePath -ceq 'REQUEST.BIN') {
            $armedHash = Get-GuBytesHash ([Text.Encoding]::ASCII.GetBytes('GUARM001' + $Receipt.SessionId + $Receipt.ProfileSha256))
            $usedHash = Get-GuBytesHash ([Text.Encoding]::ASCII.GetBytes('GUSED001' + $Receipt.SessionId + $Receipt.ProfileSha256))
            if ($f.Sha256 -cne $armedHash -or $f.AlternateSha256 -cne $usedHash) { throw 'Receipt request identity mismatch.' }
        } elseif ($f.AlternateSha256) { throw 'Only the exact consumed request may have an alternate hash.' }
    }
    $matches = 0
    foreach ($f in $files) {
        if ($f.RelativePath -cne 'BOOTX64.EFI') { continue }
        $testPlan = [pscustomobject]@{ Esp = $Receipt.Esp; SessionId = $Receipt.SessionId; EfiRelativePath = $f.RelativePath }
        $expected = New-GuEfiLoadOption $testPlan ([Text.Encoding]::ASCII.GetBytes('GUSSN001' + $Receipt.SessionId + $Receipt.ProfileSha256))
        if ([Convert]::ToBase64String($expected) -ceq $Receipt.BootBytesBase64) { $matches++ }
    }
    if ($matches -ne 1) { throw 'Receipt option does not identify its own session helper.' }
}

function Undo-GuBootStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Receipt, [hashtable]$Adapter, [switch]$AlreadyLocked, $MountedEsp)
    Assert-GuReceipt $Receipt
    if (-not $Adapter) { $Adapter = New-GuNativeBootAdapter }
    $lock = $null; $mount = $MountedEsp; $ownMount = $false; $owned = $false
    try {
        if (-not $AlreadyLocked) { $lock = & $Adapter.Lock }
        $stored = & $Adapter.ReadReceipt
        if ($null -eq $stored) { throw 'The recovery journal does not own this session.' }
        Assert-GuReceipt $stored
        foreach ($key in @('SessionId','BundleId','BundleSha256','ProfileSha256','BootName','BootBytesBase64','NextBytesBase64')) {
            if ([string]$stored.$key -cne [string]$Receipt.$key) { throw 'The recovery journal does not own this session.' }
        }
        foreach ($key in @('DiskNumber','PartitionNumber','PartitionGuid','Offset','Size','LogicalSectorSize')) {
            if ([string]$stored.Esp.$key -cne [string]$Receipt.Esp.$key) { throw 'The recovery journal does not own this session.' }
        }
        if (($stored.Files | ConvertTo-Json -Depth 4 -Compress) -cne ($Receipt.Files | ConvertTo-Json -Depth 4 -Compress)) { throw 'The recovery journal does not own this session.' }
        $owned = $true
        $option = [Convert]::FromBase64String($Receipt.BootBytesBase64)
        $next = [Convert]::FromBase64String($Receipt.NextBytesBase64)
        $entry = & $Adapter.ReadVariable $Receipt.BootName
        if ($null -ne $entry -and -not (Test-GuVariableEqual $entry $option)) { throw 'The boot option belongs to another owner now. Nothing was removed.' }
        $pending = & $Adapter.ReadVariable 'BootNext'
        if (Test-GuVariableEqual $pending $next) {
            if ($null -eq $entry) { throw 'Pending selection has no matching owned entry. Manual inspection is required.' }
            & $Adapter.DeleteVariable 'BootNext'
            if ($null -ne (& $Adapter.ReadVariable 'BootNext')) { throw 'BootNext removal could not be verified.' }
        }
        elseif ($null -ne $pending -and ([byte[]]$pending.Bytes).Length -eq 2 -and [BitConverter]::ToUInt16([byte[]]$pending.Bytes, 0) -eq [BitConverter]::ToUInt16($next, 0)) {
            throw 'Pending selection references this slot with unexpected attributes; nothing was removed.'
        }
        # Another owner's BootNext is preserved. Our option is removed only if not a normal boot target.
        $order = & $Adapter.ReadVariable 'BootOrder'
        if ($null -ne $order) {
            $orderBytes = [byte[]]$order.Bytes
            if ($orderBytes.Length % 2) { throw 'Malformed BootOrder; manual inspection is required.' }
            for ($i = 0; $i -lt $orderBytes.Length; $i += 2) {
                if ([BitConverter]::ToUInt16($orderBytes, $i) -eq [BitConverter]::ToUInt16($next, 0)) { throw 'The session option was added to BootOrder externally. It has not been deleted.' }
            }
        }
        if ($null -ne $entry) {
            if (-not (Test-GuVariableEqual (& $Adapter.ReadVariable $Receipt.BootName) $option)) { throw 'Boot option changed during cleanup.' }
            & $Adapter.DeleteVariable $Receipt.BootName
            if ($null -ne (& $Adapter.ReadVariable $Receipt.BootName)) { throw 'Boot option removal could not be verified.' }
        }
        if ($null -eq $mount) { $mount = & $Adapter.MountEsp $Receipt.Esp; $ownMount = $true }
        # Verify every existing file before deleting any; never recursively remove an unrecognized tree.
        foreach ($f in $Receipt.Files) {
            $hash = & $Adapter.GetFileHash $mount $Receipt.SessionId $f.RelativePath
            if ($null -ne $hash -and $hash -cne $f.Sha256 -and $hash -cne $f.AlternateSha256) { throw ('Staged file changed; preserving it: ' + $f.RelativePath) }
        }
        foreach ($f in $Receipt.Files) { & $Adapter.DeleteFile $mount $Receipt.SessionId $f.RelativePath @($f.Sha256, $f.AlternateSha256) }
        $empty = & $Adapter.DeleteSessionDirectory $mount $Receipt.SessionId
        if ($empty -eq $false) {
            $Receipt.State = 'CancelledFilesRemain'; $Receipt.LastError = 'One-time activation is cancelled. Generated logs or unrecognized files remain in the session folder.'
            & $Adapter.WriteReceipt $Receipt
            & $Adapter.ClearReceipt $Receipt.SessionId # Archive terminal record; retain only logs on ESP.
            return [pscustomobject]@{ State = 'CancelledFilesRemain'; SessionId = $Receipt.SessionId; RetainedFolder = ('EFI/GpuUnlockSession/' + $Receipt.SessionId) }
        }
        $Receipt.State = 'Removed'; $Receipt.LastError = ''; & $Adapter.WriteReceipt $Receipt
        & $Adapter.ClearReceipt $Receipt.SessionId
        return [pscustomobject]@{ State = 'Removed'; SessionId = $Receipt.SessionId }
    } catch {
        if ($owned) {
            $Receipt.State = 'RecoveryNeeded'; $Receipt.LastError = $_.Exception.Message
            try { & $Adapter.WriteReceipt $Receipt } catch { }
        }
        throw
    } finally {
        try { if ($ownMount -and $null -ne $mount) { & $Adapter.UnmountEsp $mount } }
        finally { if ($null -ne $lock) { & $Adapter.Unlock $lock } }
    }
}

function Get-GuPendingSession {
    param([hashtable]$Adapter)
    if ($Adapter) { return (& $Adapter.ReadReceipt) }
    $directory = Join-Path $env:ProgramData 'GpuUnlockSession'
    $path = Join-Path $directory 'pending.json'
    Assert-GuTrustedJournalStore $directory $path
    if (-not [IO.File]::Exists($path)) { return $null }
    if ((Get-Item -LiteralPath $path).Length -gt 131072) { throw 'Recovery journal is too large.' }
    $receipt = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    Assert-GuReceipt $receipt
    return $receipt
}

function Assert-GuNativeWindows {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) { throw 'An elevated 64-bit Windows process is required.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        if (-not (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator access is required for boot scheduling.' }
    } finally { $identity.Dispose() }
}

function Assert-GuNativePath {
    param([string]$Path)
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if ([IO.File]::Exists($current) -or [IO.Directory]::Exists($current)) {
            if (([IO.File]::GetAttributes($current) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse points are not accepted for session storage.' }
        }
        $parent = [IO.Path]::GetDirectoryName($current.TrimEnd('\'))
        if ($parent -eq $current) { break }; $current = $parent
    }
}

function Assert-GuTrustedAcl {
    param([string]$Path)
    $acl = Get-Acl -LiteralPath $Path -ErrorAction Stop
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin @('S-1-5-18','S-1-5-32-544') -or -not $acl.AreAccessRulesProtected) {
        throw 'The session journal is not owned and protected by Administrators or SYSTEM. It will not be trusted.'
    }
    $writeMask = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            $rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544') -and
            ($rule.FileSystemRights -band $writeMask) -ne 0) {
            throw 'The session journal grants write or delete access to an untrusted principal. It will not be trusted.'
        }
    }
}

function Assert-GuTrustedJournalStore {
    param([string]$Directory, [string]$File)
    Assert-GuNativePath $Directory
    Assert-GuNativePath $File
    if ([IO.Directory]::Exists($Directory)) { Assert-GuTrustedAcl $Directory }
    if ([IO.File]::Exists($File)) { Assert-GuTrustedAcl $File }
}

function New-GuNativeBootAdapter {
    Assert-GuNativeWindows
    if (-not ('GpuUnlock.Session.FirmwareVariables' -as [type])) { Add-Type -Path (Join-Path $script:BackendRoot 'FirmwareVariables.cs') }
    $stateDirectory = Join-Path $env:ProgramData 'GpuUnlockSession'
    $statePath = Join-Path $stateDirectory 'pending.json'
    $assertPath = ${function:Assert-GuNativePath}
    $assertRelative = ${function:Assert-GuRelativePath}
    $getEsp = ${function:Get-GuSystemEsp}
    $assertStore = ${function:Assert-GuTrustedJournalStore}
    $assertAcl = ${function:Assert-GuTrustedAcl}
    $resolve = {
        param($mount, [string]$sessionId, [string]$relative)
        if ($sessionId -cnotmatch '\A[0-9a-f]{32}\z') { throw 'Invalid session directory name.' }
        $root = Join-Path $mount.RootPath ('EFI\GpuUnlockSession\' + $sessionId)
        $path = $root
        if ($relative) { & $assertRelative $relative; $path = Join-Path $root $relative.Replace('/', '\') }
        & $assertPath $path
        return $path
    }.GetNewClosure()
    return @{
        Kind = 'Native'
        Lock = {
            $mutex = New-Object Threading.Mutex($false, 'Global\GpuUnlockSessionManager')
            try {
                try { $held = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
                if (-not $held) { throw 'Another session manager operation is running.' }
                return $mutex
            } catch { $mutex.Dispose(); throw }
        }
        Unlock = { param($mutex) try { $mutex.ReleaseMutex() } finally { $mutex.Dispose() } }
        ReadVariable = { param($name) return [GpuUnlock.Session.FirmwareVariables]::Read($name) }
        WriteVariable = { param($name, [byte[]]$bytes) [GpuUnlock.Session.FirmwareVariables]::Write($name, $bytes) }
        DeleteVariable = { param($name) [GpuUnlock.Session.FirmwareVariables]::Delete($name) }
        MountEsp = {
            param($expected)
            $now = & $getEsp
            foreach ($key in @('DiskNumber', 'PartitionNumber', 'PartitionGuid', 'Offset', 'Size', 'LogicalSectorSize')) {
                if ([string]$now.$key -cne [string]$expected.$key) { throw 'System EFI partition changed since the plan was made.' }
            }
            $letter = $null
            foreach ($candidate in @('Z','Y','X','W','V','U','T','S','R')) {
                if (-not (Test-Path -LiteralPath ($candidate + ':\')) -and -not (Get-PSDrive -Name $candidate -ErrorAction SilentlyContinue)) { $letter = $candidate; break }
            }
            if (-not $letter) { throw 'No temporary drive letter is available.' }
            $root = $letter + ':\'; $mountvol = Join-Path $env:SystemRoot 'System32\mountvol.exe'
            $null = & $mountvol ($letter + ':') /S
            if ($LASTEXITCODE -ne 0) { throw 'Could not mount the system EFI partition.' }
            try {
                $p = Get-Partition -DriveLetter $letter -ErrorAction Stop
                $v = Get-Volume -DriveLetter $letter -ErrorAction Stop
                if ($p.DiskNumber -ne $expected.DiskNumber -or $p.PartitionNumber -ne $expected.PartitionNumber -or
                    ([guid]$p.Guid).ToString('D') -cne $expected.PartitionGuid -or $v.FileSystem -ne 'FAT32') { throw 'Mounted partition does not match the reviewed FAT32 ESP.' }
                return [pscustomobject]@{ RootPath = $root; DriveLetter = $letter; FreeBytes = [uint64]$v.SizeRemaining }
            } catch { $null = & $mountvol ($letter + ':') /D; throw }
        }.GetNewClosure()
        UnmountEsp = {
            param($mount)
            $null = & (Join-Path $env:SystemRoot 'System32\mountvol.exe') ($mount.DriveLetter + ':') /D
            if ($LASTEXITCODE -ne 0) { Write-Warning ('Temporary EFI mount remains at ' + $mount.RootPath) }
        }
        CreateSessionDirectory = {
            param($mount, $sessionId)
            $path = & $resolve $mount $sessionId ''
            if ([IO.Directory]::Exists($path) -or [IO.File]::Exists($path)) { throw 'Session directory already exists.' }
            $null = [IO.Directory]::CreateDirectory($path)
        }.GetNewClosure()
        PutFile = {
            param($mount, $sessionId, $relative, [byte[]]$bytes)
            $path = & $resolve $mount $sessionId $relative
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
            $file = New-Object IO.FileStream($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
        }.GetNewClosure()
        GetFileHash = {
            param($mount, $sessionId, $relative)
            $path = & $resolve $mount $sessionId $relative
            if (-not [IO.File]::Exists($path)) { return $null }
            return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }.GetNewClosure()
        DeleteFile = {
            param($mount, $sessionId, $relative, [string[]]$expectedHashes)
            $path = & $resolve $mount $sessionId $relative
            if ([IO.File]::Exists($path)) {
                if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cnotin $expectedHashes) { throw 'File changed during cleanup.' }
                [IO.File]::Delete($path)
            }
        }.GetNewClosure()
        DeleteSessionDirectory = {
            param($mount, $sessionId)
            $path = & $resolve $mount $sessionId ''
            if (-not [IO.Directory]::Exists($path)) { return $true }
            if (@(Get-ChildItem -LiteralPath $path -File -Recurse -Force).Count -gt 0) { return $false }
            # Only empty directories are removed; unknown files block cleanup.
            $dirs = @(Get-ChildItem -LiteralPath $path -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending)
            foreach ($dir in $dirs) { & $assertPath $dir.FullName; [IO.Directory]::Delete($dir.FullName, $false) }
            [IO.Directory]::Delete($path, $false)
            return $true
        }.GetNewClosure()
        WriteReceipt = {
            param($receipt)
            & $assertStore $stateDirectory $statePath
            [GpuUnlock.Session.ProtectedJournal]::CreateDirectory($stateDirectory)
            & $assertAcl $stateDirectory
            $temp = Join-Path $stateDirectory ('journal-' + [guid]::NewGuid().ToString('N') + '.tmp')
            $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($receipt | ConvertTo-Json -Depth 8))
            [GpuUnlock.Session.ProtectedJournal]::WriteNewFile($temp, $bytes)
            & $assertAcl $temp
            if ([IO.File]::Exists($statePath)) { [IO.File]::Replace($temp, $statePath, $null) }
            else { [IO.File]::Move($temp, $statePath) }
            & $assertStore $stateDirectory $statePath
        }.GetNewClosure()
        ReadReceipt = {
            & $assertStore $stateDirectory $statePath
            if (-not [IO.File]::Exists($statePath)) { return $null }
            if ((Get-Item -LiteralPath $statePath).Length -gt 131072) { throw 'Recovery journal is too large.' }
            return ([IO.File]::ReadAllText($statePath) | ConvertFrom-Json)
        }.GetNewClosure()
        ClearReceipt = {
            param($sessionId)
            & $assertStore $stateDirectory $statePath
            if ([IO.File]::Exists($statePath)) {
                $current = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json
                if ($current.SessionId -cne $sessionId -or $current.State -cnotin @('Removed', 'CancelledFilesRemain')) { throw 'Journal changed during cleanup.' }
                if ($current.State -ceq 'CancelledFilesRemain') {
                    $archiveDirectory = Join-Path $stateDirectory 'archives'
                    & $assertPath $archiveDirectory
                    if ([IO.Directory]::Exists($archiveDirectory)) { & $assertAcl $archiveDirectory }
                    [GpuUnlock.Session.ProtectedJournal]::CreateDirectory($archiveDirectory)
                    & $assertAcl $archiveDirectory
                    $archive = Join-Path $archiveDirectory ($sessionId + '.json')
                    & $assertPath $archive
                    if (-not [IO.File]::Exists($archive)) { [GpuUnlock.Session.ProtectedJournal]::WriteNewFile($archive, [IO.File]::ReadAllBytes($statePath)) }
                    & $assertAcl $archive
                    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash) { throw 'Terminal journal archive verification failed.' }
                }
                [IO.File]::Delete($statePath)
            }
        }.GetNewClosure()
    }
}

Export-ModuleMember -Function New-GuBootPlan, Invoke-GuBootStage, Undo-GuBootStage, Get-GuPendingSession, Get-GuSystemEsp, New-GuNativeBootAdapter, New-GuEfiLoadOption
