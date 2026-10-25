# SPDX-License-Identifier: GPL-2.0-only
# Standard read-only OS/driver queries. The only write is this local report.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Run this collector in Windows after the USB test.' }
$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('GPU Unlock 0.6.4 Windows observation (read-only; no performance claim)')
$lines.Add('UTC: ' + [DateTime]::UtcNow.ToString('o'))
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $lines.Add(($os | Select-Object Caption,Version,BuildNumber,OSArchitecture | Format-List | Out-String).Trim())
} catch { $lines.Add('OS query failed: ' + $_.Exception.Message) }
try {
    $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop)
    $lines.Add(($gpus | Select-Object Name,DriverVersion,PNPDeviceID | Format-List | Out-String).Trim())
} catch { $lines.Add('Video-controller query failed: ' + $_.Exception.Message) }
$smi = Get-Command nvidia-smi.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$executable = if ($null -ne $smi) { $smi.Source } else { Join-Path $env:WINDIR 'System32/nvidia-smi.exe' }
if (Test-Path -LiteralPath $executable -PathType Leaf) {
    $lines.Add('NVIDIA query columns: index,name,pci.bus_id,uuid,driver_version,memory.total')
    try {
        $result = & $executable '--query-gpu=index,name,pci.bus_id,uuid,driver_version,memory.total' '--format=csv,noheader' 2>&1
        $exitCode = $LASTEXITCODE
        $lines.Add(($result | Out-String).Trim()); $lines.Add('nvidia-smi exit: ' + $exitCode)
    } catch { $lines.Add('nvidia-smi query failed: ' + $_.Exception.Message) }
} else { $lines.Add('nvidia-smi.exe was not found. The installed driver was not changed.') }
$name = 'Windows-Result-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8) + '.txt'
$path = Join-Path $PSScriptRoot $name
$bytes = [Text.UTF8Encoding]::new($false).GetBytes(($lines -join [Environment]::NewLine) + [Environment]::NewLine)
$stream = [IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
Write-Host ('Saved: ' + $path)
Write-Host 'Send this file and the newest USB REPORT.TXT. These queries do not prove an unlock or performance gain.'
