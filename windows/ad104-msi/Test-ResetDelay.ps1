param([string]$Gcc='gcc')
$ErrorActionPreference='Stop'
$build=Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-fshort-wchar','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL')
foreach($path in @('compat','vendor/edk2/Include','vendor/edk2/Include/X64')) {
    $flags+=('-I'+(Join-Path $PSScriptRoot $path))
}
# Both executables use synthetic EFI effects. No PROM, payload, driver image,
# Windows image or GPU access is needed; no EFI application is built or run.
foreach($test in @(
    @{Name='reset-delay';Sources=@('tests/test_reset_delay.c','port.c','compat.c','sha256.c','format.c')},
    @{Name='app-flow';Sources=@('tests/test_app_flow.c')}
)) {
    $sources=@($test.Sources | ForEach-Object { Join-Path $PSScriptRoot $_ })
    $exe=Join-Path $build ('test-'+$test.Name+'.exe')
    & $Gcc @flags @sources -o $exe
    if($LASTEXITCODE -ne 0){throw ('Compile failed: '+$test.Name)}
    & $exe
    if($LASTEXITCODE -ne 0){throw ('Test failed: '+$test.Name)}
}
