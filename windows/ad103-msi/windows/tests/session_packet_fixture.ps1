# SPDX-License-Identifier: GPL-2.0-only
# Synthetic cross-language wire fixture. No ESP or firmware-variable access.
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidatePattern('\A[0-9a-f]{64}\z')][string]$ProfileSha256
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Fixed fake geometry and session ID; these packets are never booted.
$sessionId = '0123456789abcdef0123456789abcdef'
$path = '\EFI\GpuUnlockSession\' + $sessionId + '\BOOTX64.EFI'
$pathBytes = [Text.Encoding]::Unicode.GetBytes($path + [char]0)
$description = [Text.Encoding]::Unicode.GetBytes('GPU Unlock session ' + $sessionId + [char]0)
$optional = [Text.Encoding]::ASCII.GetBytes('GUSSN001' + $sessionId + $ProfileSha256)
$request = [Text.Encoding]::ASCII.GetBytes('GUARM001' + $sessionId + $ProfileSha256)
$stream = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter($stream)
try {
    $writer.Write([uint32]1) # LOAD_OPTION_ACTIVE
    $writer.Write([uint16](42 + 4 + $pathBytes.Length + 4))
    $writer.Write($description)
    $writer.Write([byte]4); $writer.Write([byte]1); $writer.Write([uint16]42) # MEDIA_HARDDRIVE_DP
    $writer.Write([uint32]1) # Partition number
    $writer.Write([uint64]2048) # Start LBA: 1 MiB / 512
    $writer.Write([uint64]204800) # Size: 100 MiB / 512
    $writer.Write(([guid]'12345678-1234-5678-9abc-123456789abc').ToByteArray())
    $writer.Write([byte]2); $writer.Write([byte]2) # GPT, GUID signature
    $writer.Write([byte]4); $writer.Write([byte]4); $writer.Write([uint16](4 + $pathBytes.Length))
    $writer.Write($pathBytes)
    $writer.Write([byte]0x7f); $writer.Write([byte]0xff); $writer.Write([uint16]4)
    $writer.Write($optional)
    [IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'boot-option.bin'), $stream.ToArray())
    [IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'request.bin'), $request)
} finally { $writer.Dispose(); $stream.Dispose() }
Write-Output 'Synthetic session packets generated in the test directory.'
