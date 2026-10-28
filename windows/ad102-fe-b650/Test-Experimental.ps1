param(
 [string]$Gcc='gcc',
 [string]$Prom,
 [Parameter(Mandatory=$true)][string]$Assets,
 [Parameter(Mandatory=$true)][string]$DriverCode,
 [Parameter(Mandatory=$true)][string]$WindowsImage,
 [switch]$IncludeCore
)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 if($IncludeCore){& .\Build.ps1 -RunTests -Prom $Prom -Assets $Assets;if(!$?){throw 'Core tests failed'}}
 $flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_PROFILE_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-D__USE_MINGW_ANSI_STDIO=1','-Icompat','-I../ad104-msi/vendor/edk2/Include','-I../ad104-msi/vendor/edk2/Include/X64')
 $cases=@(
  @{Name='app-flow';Source='tests/test_app_flow.c';Extra=@();Args=@()},
  @{Name='msi-release';Source='tests/test_msi_release.c';Extra=@('sha256.c','format.c');Args=@($DriverCode)},
  @{Name='boot-io';Source='tests/test_boot_io.c';Extra=@('sha256.c','format.c');Args=@($WindowsImage,$Assets)}
 )
 foreach($case in $cases) {
  $exe='build/test-'+$case.Name+'.exe';$log='build/'+$case.Name+'.log'
  & $Gcc @flags $case.Source @($case.Extra) -o $exe
  if($LASTEXITCODE -ne 0){throw ('Compile failed: '+$case.Name)}
  & ('.\'+($exe -replace '/','\')) @($case.Args) > $log 2>&1
  if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log -Tail 25;throw ('Test failed: '+$case.Name)}
  Get-Content -LiteralPath $log -Tail 1
 }
} finally {Pop-Location}
