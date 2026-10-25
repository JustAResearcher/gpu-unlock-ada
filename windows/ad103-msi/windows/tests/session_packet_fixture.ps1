# SPDX-License-Identifier: GPL-2.0-only
# Cross-language fixture generator. Every mutation is intercepted by this fake adapter.
param(
    [Parameter(Mandatory)][string]$ModulePath,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$ProfileSha256
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module $ModulePath -Force
$null = [IO.Directory]::CreateDirectory($OutputDirectory)
$payload = Join-Path $OutputDirectory 'SYNTHETIC-NOT-AN-EFI.bin'
[IO.File]::WriteAllBytes($payload, [byte[]](77,90,1,2,3,4))
function Hash([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
$snapshot = [pscustomobject]@{ Esp = [pscustomobject]@{
    DiskNumber=0; PartitionNumber=1; PartitionGuid='12345678-1234-5678-9abc-123456789abc'
    Offset=[uint64]1048576; Size=[uint64]104857600; LogicalSectorSize=512
} }
$bundle = [pscustomobject]@{
    Id='synthetic-cross-language-test'; ManifestSha256=('a'*64); ProfileSha256=$ProfileSha256
    HelperProtocol='gu-session-v1'; Validated=$true; EfiRelativePath='BOOTX64.EFI'
    Files=@([pscustomobject]@{SourcePath=$payload; RelativePath='BOOTX64.EFI'; Sha256=(Hash ([IO.File]::ReadAllBytes($payload)))})
}
$plan = New-GuBootPlan $snapshot $bundle
$state = @{Vars=@{BootOrder=[pscustomobject]@{Attributes=7;Bytes=[byte[]](1,0)}};Files=@{};Receipt=$null}
$adapter = @{
    Kind='Fake'
    Lock={ return 'fake-lock' }
    Unlock={param($lock)}
    ReadReceipt={return $state.Receipt}
    WriteReceipt={param($receipt) $state.Receipt=$receipt}
    ClearReceipt={param($id) $state.Receipt=$null}
    ReadVariable={param($name) if($state.Vars.ContainsKey($name)){return $state.Vars[$name]};return $null}
    WriteVariable={param($name,[byte[]]$bytes) $state.Vars[$name]=[pscustomobject]@{Attributes=7;Bytes=$bytes.Clone()}}
    DeleteVariable={param($name) $state.Vars.Remove($name)}
    MountEsp={param($esp) return [pscustomobject]@{RootPath='fake:/';FreeBytes=[uint64]1073741824}}
    UnmountEsp={param($mount)}
    CreateSessionDirectory={param($mount,$id)}
    PutFile={param($mount,$id,$relative,[byte[]]$bytes) $state.Files[$relative]=$bytes.Clone()}
    GetFileHash={param($mount,$id,$relative) if($state.Files.ContainsKey($relative)){return (Hash $state.Files[$relative])};return $null}
    DeleteFile={param($mount,$id,$relative,$hashes) $state.Files.Remove($relative)}
    DeleteSessionDirectory={param($mount,$id) return $true}
}
# The actual production staging function generates both wire packets.
$receipt = Invoke-GuBootStage $plan -AuthorizePlan {$true} -Adapter $adapter
if($receipt.State -cne 'Scheduled'){throw 'Fixture staging did not complete.'}
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'boot-option.bin'),[byte[]]$state.Vars[$receipt.BootName].Bytes)
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'request.bin'),[byte[]]$state.Files['REQUEST.BIN'])
Write-Output 'Synthetic packets generated through fake-adapter production staging.'
