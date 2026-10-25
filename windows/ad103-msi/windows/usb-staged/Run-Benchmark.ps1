# SPDX-License-Identifier: GPL-2.0-only
# Launch a user-mode compute measurement. No elevation or persistent settings.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('stock','unlocked','compare')][string]$Mode,
    [string]$GpuUuid = ''
)
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Run the benchmark launcher in Windows.'
}
$runner = Join-Path $PSScriptRoot 'benchmark\run_benchmark.py'
$results = Join-Path $PSScriptRoot 'benchmark-results'
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw 'Extract the complete ZIP first; benchmark\run_benchmark.py is missing.'
}
if ($Mode -ne 'compare') {
    if ([string]::IsNullOrWhiteSpace($GpuUuid)) {
        Write-Host 'Run nvidia-smi -L in another terminal and copy the full GPU-... UUID of the intended GPU.'
        Write-Host 'Select that same GPU for both measurements. There is no default GPU.'
        $GpuUuid = Read-Host 'GPU UUID'
    }
    if ($GpuUuid -notmatch '\AGPU-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\z') {
        throw 'A full GPU-... UUID is required. GPU indices, partial UUIDs and an empty selection are not accepted.'
    }
    $GpuUuid = 'GPU-' + $GpuUuid.Substring(4).ToLowerInvariant()
}

# Check available interpreters without installing anything or importing packages.
# The output sentinel also rejects the Windows Store alias and 32-bit Python.
$pythonExe = $null
$pythonPrefix = @()
# Python single-quoted literals survive Windows PowerShell 5's legacy native
# argument marshalling; embedded double quotes can be stripped there.
$probe = 'import sys,struct; print(''GU_PYTHON_OK'' if sys.version_info >= (3,9) and struct.calcsize(''P'')==8 else ''UNSUPPORTED'')'
foreach ($name in @('py.exe','python.exe','python3.exe')) {
    $candidate = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $candidate) { continue }
    if ($name -ne 'py.exe' -and $candidate.Source -match '[\\/]WindowsApps[\\/]') { continue }
    $prefix = @()
    if ($name -eq 'py.exe') { $prefix = @('-3') }
    try {
        $answer = @(& $candidate.Source @prefix '-I' '-c' $probe 2>$null)
        if ($LASTEXITCODE -eq 0 -and $answer.Count -eq 1 -and $answer[0] -ceq 'GU_PYTHON_OK') {
            $pythonExe = $candidate.Source
            $pythonPrefix = $prefix
            break
        }
    } catch { continue }
}
if ($null -eq $pythonExe) {
    throw '64-bit Python 3.9 or newer was not found. Use the Python installation from the earlier USB asset setup, or install 64-bit Python from https://www.python.org/downloads/windows/ and rerun. No packages, PyTorch or CUDA Toolkit are required.'
}
$arguments = @('-I', $runner)
if ($Mode -eq 'compare') {
    $stock = Get-ChildItem -LiteralPath $results -Filter 'stock-*.json' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $unlocked = Get-ChildItem -LiteralPath $results -Filter 'unlocked-*.json' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $stock -or $null -eq $unlocked) {
        throw 'Both a Stock and an Unlocked result are required in benchmark-results. Run each launcher in its corresponding boot session, using this same extracted folder.'
    }
    Write-Host ('Stock:    ' + $stock.FullName)
    Write-Host ('Unlocked: ' + $unlocked.FullName)
    $arguments += @('compare', $stock.FullName, $unlocked.FullName, '--output-dir', $results)
} else {
    if ($Mode -eq 'stock') {
        Write-Host 'STOCK label: run after a full power-off/cold start with the unlock USB removed.'
        Write-Host 'A warm restart after an unlock is not a reliable stock baseline.'
    } else {
        Write-Host 'UNLOCKED label: run immediately after the unlock boots Windows; do not reboot first.'
    }
    Write-Host 'Close games and other GPU workloads first. This runs a short compute workload.'
    Write-Host 'The label describes your boot session; it does not itself verify the unlock.'
    $arguments += @('run', '--label', $Mode, '--gpu-uuid', $GpuUuid, '--output-dir', $results)
}
& $pythonExe @pythonPrefix @arguments
if ($LASTEXITCODE -ne 0) { throw ('Benchmark stopped with exit code ' + $LASTEXITCODE + '. See the message above.') }
Write-Host ('Results: ' + $results)
