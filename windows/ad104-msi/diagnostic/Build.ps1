param(
    [string]$VcVars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    [string]$Python = 'python',
    [ValidateSet('ad104-msi','ad102-fe')][string]$Target = 'ad104-msi'
)
$ErrorActionPreference = 'Stop'
$targets = @{
    'ad104-msi' = @{ Build='build'; Binary='AD104-PROBE.EFI'; Define='' }
    'ad102-fe' = @{ Build='build-ad102-fe'; Binary='AD102-FE-PROBE.EFI'; Define='/DPROBE_TARGET_AD102_FE' }
}
$selected = $targets[$Target]
$env:PROBE_TARGET = $Target
if (-not (Test-Path -LiteralPath $VcVars)) { throw "Missing MSVC setup: $VcVars" }
if ($VcVars -match '["&|<>%^!\r\n]' -or $PSScriptRoot -match '["&|<>%^!\r\n]') { throw 'Unsupported command-shell characters in build paths' }
$buildDir = Join-Path $PSScriptRoot $selected.Build
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$sourceHashes = [ordered]@{}
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src') -File | Sort-Object Name | ForEach-Object { $sourceHashes[$_.Name] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
$identityText = (($sourceHashes.GetEnumerator() | ForEach-Object { $_.Name + '=' + $_.Value }) -join "`n")
$identityText += "`nheaders=" + (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '..\vendor\edk2\headers.json') -Algorithm SHA256).Hash
$identityText += "`nbuild=" + (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
$identityText += "`ntarget=" + $Target
$hasher = [Security.Cryptography.SHA256]::Create()
try { $buildId = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($identityText))).Replace('-','').ToLowerInvariant() } finally { $hasher.Dispose() }
Set-Content -LiteralPath (Join-Path $buildDir 'build_id.h') -Value ('#define PROBE_BUILD_ID "' + $buildId + '"') -Encoding ascii
$script = @'
@echo off
call "@VCVARS@" >nul
if errorlevel 1 exit /b 1
cd /d "@ROOT@"
cl /nologo /W4 /WX @DEFINE@ /O2 /Oi /GS- /Gs65536 /Zl /c /TC /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 /Fo@BUILD@\core.obj src\core.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX @DEFINE@ /O2 /Oi /GS- /Gs65536 /Zl /c /TC /FIbuild_id.h /I@BUILD@ /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 /Fo@BUILD@\probe.obj src\probe.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX @DEFINE@ /O2 /Oi /GS- /Gs65536 /Zl /c /TC /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 /Fo@BUILD@\sample.obj src\sample.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX @DEFINE@ /O2 /Oi /GS- /Gs65536 /Zl /c /TC /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 /Fo@BUILD@\platform.obj src\platform.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX @DEFINE@ /Od /Oi- /GS- /Zl /c /TC /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 /Fo@BUILD@\runtime.obj src\runtime.c
if errorlevel 1 exit /b 1
link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /out:@BUILD@\@BINARY@ @BUILD@\core.obj @BUILD@\probe.obj @BUILD@\sample.obj @BUILD@\platform.obj @BUILD@\runtime.obj
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX @DEFINE@ /Od /D_CRT_SECURE_NO_WARNINGS /FIbuild_id.h /I@BUILD@ /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 tests\host_tests.c src\core.c src\sample.c src\platform.c /Fe@BUILD@\host-tests.exe /Fo@BUILD@\
if errorlevel 1 exit /b 1
@BUILD@\host-tests.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX @DEFINE@ /Od /D_CRT_SECURE_NO_WARNINGS /I..\vendor\edk2\Include /I..\vendor\edk2\Include\X64 tests\platform_tests.c src\core.c src\platform.c /Fe@BUILD@\platform-tests.exe /Fo@BUILD@\
if errorlevel 1 exit /b 1
@BUILD@\platform-tests.exe
if errorlevel 1 exit /b 1
'@
$script = $script.Replace('@VCVARS@', $VcVars).Replace('@ROOT@', $PSScriptRoot).Replace('@BUILD@', $selected.Build).Replace('@DEFINE@', $selected.Define).Replace('@BINARY@', $selected.Binary)
$cmdFile = Join-Path $buildDir 'compile.cmd'
Set-Content -LiteralPath $cmdFile -Value $script -Encoding ascii
& $env:ComSpec /d /c $cmdFile
if ($LASTEXITCODE -ne 0) { throw 'Compiler or host test failure' }
& $Python (Join-Path $PSScriptRoot 'tools\verify.py')
if ($LASTEXITCODE -ne 0) { throw 'PE/source verification failure' }
& $Python -m unittest discover -s (Join-Path $PSScriptRoot 'tests') -p 'test_*.py' -v
if ($LASTEXITCODE -ne 0) { throw 'Python verification test failure' }
$binary = Join-Path $buildDir $selected.Binary
$manifest = [ordered]@{ version='0.4.0'; target=$Target; build_id=$buildId; binary=$selected.Binary; sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant(); source_sha256=$sourceHashes; hardware_tested=$false; gpu_write_capability=$false; mmio_read_capability=$true; validation='MSVC host mocks and PE checks'; utc=[DateTime]::UtcNow.ToString('o') }
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $buildDir 'manifest.json') -Encoding utf8
Write-Output $binary
