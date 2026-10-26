param(
 [string]$Gcc='gcc',
 [Parameter(Mandatory=$true)][string]$Assets,
 [Parameter(Mandatory=$true)][string]$WindowsImage,
 [ValidateSet('All','Boot','App')][string]$Suite='All',
 [string]$CaseName,
 [string]$EvidenceDirectory,
 [switch]$CompileOnly
)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$Assets=(Resolve-Path -LiteralPath $Assets).Path
$WindowsImage=(Resolve-Path -LiteralPath $WindowsImage).Path
if(!$EvidenceDirectory){$EvidenceDirectory=Join-Path $PSScriptRoot 'build-boot4070s/evidence'}
$EvidenceDirectory=[IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$testProcess=[Diagnostics.Process]::GetCurrentProcess()
$previousPriority=$testProcess.PriorityClass
$testProcess.PriorityClass='BelowNormal'
Push-Location -LiteralPath $PSScriptRoot
try {
 New-Item -ItemType Directory -Path 'build-boot4070s' -Force | Out-Null
 $flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
  '-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL',
  '-Icompat','-Ivendor/edk2/Include','-Ivendor/edk2/Include/X64','-ffunction-sections','-fdata-sections')
 # The validator remains the actual production function. Renaming the unused
 # prepare/cold symbols lets app-flow simulate those separately tested seams;
 # every hardware dependency of the unused functions has a fail-fast host trap.
 $profileArgs=$flags+@('-DGuAd104Prepare=Boot4070sUnusedPrepare','-DGuAd104ColdEntry=Boot4070sUnusedColdEntry','-c','profile.c','-o','build-boot4070s/profile.o')
 & $Gcc @profileArgs *> (Join-Path $EvidenceDirectory 'compile-profile.log')
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $EvidenceDirectory 'compile-profile.log');throw 'COMPILE_ERROR: actual profile.c validator object'}
 $cases=@(
  @{Name='boot-io';Selection='Boot';Source='tests/test_boot_io.c'},
  @{Name='app-flow';Selection='App';Source='tests/test_app_flow.c'}
 )
 $results=@()
 foreach($case in $cases) {
  if($Suite -ne 'All' -and $Suite -ne $case.Selection){continue}
  $exe='build-boot4070s/test-'+$case.Name+'.exe'
  $compileArgs=$flags+@($case.Source,'build-boot4070s/profile.o','sha256.c','format.c','-Wl,--gc-sections','-o',$exe)
  $compileLog=Join-Path $EvidenceDirectory ('compile-'+$case.Name+'.log')
  & $Gcc @compileArgs *> $compileLog
  $compileExit=$LASTEXITCODE
  $result=[ordered]@{suite=$case.Name;compile_exit=$compileExit;compile_args=$compileArgs;run_exit=$null;run_args=$null;log=$null}
  if($compileExit -ne 0){Get-Content -LiteralPath $compileLog;$results+=$result;continue}
  if(!$CompileOnly) {
   $runArgs=@($WindowsImage,$Assets)
   if($CaseName){$runArgs+=$CaseName}
   $log=Join-Path $EvidenceDirectory ($case.Name+'.log')
   & (Join-Path $PSScriptRoot $exe) @runArgs *> $log
   $result.run_exit=$LASTEXITCODE;$result.run_args=$runArgs;$result.log=$log
   Get-Content -LiteralPath $log -Tail 6
  }
  $results+=$result
 }
 $hashes=[ordered]@{}
 foreach($file in @('tests/test_boot_io.c','tests/test_app_flow.c','tests/boot4070s_config.h','tests/boot4070s_fixtures.h','tests/boot4070s_profile_traps.h','Test-Boot4070S.ps1','boot_io.c','app.c','boot_profile.h','profile.c','port.h','sha256.c')) {
  $hashes[$file]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
 }
 $summary=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');compiler=(Get-Command $Gcc).Source;profile_compile_args=$profileArgs;
  priority='BelowNormal';compile_only=[bool]$CompileOnly;results=$results;source_sha256=$hashes;hardware_access=$false;private_bytes_uploaded=$false}
 $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'run.json') -Encoding utf8
 $compileFailures=@($results | Where-Object {$_.compile_exit -ne 0})
 $fixtureFailures=@($results | Where-Object {$null -ne $_.run_exit -and $_.run_exit -ne 0 -and $_.run_exit -ne 1})
 $semanticFailures=@($results | Where-Object {$_.run_exit -eq 1})
 if($compileFailures.Count){throw 'COMPILE_ERROR: inspect compile logs; this is not semantic RED'}
 if($fixtureFailures.Count){throw 'FIXTURE_OR_RUNTIME_ERROR: inspect logs; this is not semantic RED'}
 if($semanticFailures.Count){throw 'SEMANTIC_RED: compiled test expectations failed; inspect named scenarios'}
 Write-Output 'Boot4070S checks passed; host simulation only, no firmware execution.'
} finally {
 Pop-Location
 $testProcess.PriorityClass=$previousPriority
}
