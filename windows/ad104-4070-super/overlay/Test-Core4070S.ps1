param(
 [string]$Gcc='gcc',
 [string]$Ar='ar',
 [string]$Python='python',
 [Parameter(Mandatory=$true)][string]$Prom,
 [Parameter(Mandatory=$true)][string]$Assets,
 [string]$Evidence,
 [string[]]$Groups,
 [switch]$CompileOnly
)
$ErrorActionPreference='Stop'
if(!$Evidence){$Evidence=Join-Path $PSScriptRoot 'build/test-evidence/core'}
$Evidence=[System.IO.Path]::GetFullPath($Evidence)
$Prom=(Resolve-Path -LiteralPath $Prom).Path
$Assets=(Resolve-Path -LiteralPath $Assets).Path
$process=[System.Diagnostics.Process]::GetCurrentProcess()
$oldPriority=$process.PriorityClass
$oldPath=$env:PATH
$commands=[System.Collections.Generic.List[object]]::new()
$results=[System.Collections.Generic.List[object]]::new()
Push-Location -LiteralPath $PSScriptRoot
try {
 $process.PriorityClass='BelowNormal'
 $env:PATH=(Split-Path -Parent (Get-Command $Gcc -CommandType Application).Source)+';'+$env:PATH
 New-Item -ItemType Directory -Path 'build/core4070s' -Force | Out-Null
 New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
 $Evidence=(Resolve-Path -LiteralPath $Evidence).Path
 if((Get-FileHash -LiteralPath $Prom -Algorithm SHA256).Hash.ToLowerInvariant() -ne '46b0ff2e697582fccefde3cbcb62eaff423bdce76f853040445030a7c5cc81fb') {
  throw 'The test requires the exact captured physical PROM.'
 }
 # Preserve the existing production build/check path; never produce or install an EFI application.
 & ./Build.ps1 -Gcc $Gcc -Ar $Ar -Python $Python *> (Join-Path $Evidence 'archive-build.log')
 $common=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-D_CRT_SECURE_NO_WARNINGS')
 $includes=@('-Icompat','-Ivendor/edk2/Include','-Ivendor/edk2/Include/X64')
 $sources=@('port.c','compat.c','checks.c','profile.c','sha256.c','vendor/unlock_610.c','vendor/prep_610.c','vendor/layout_610.c','vendor/vbios_610.c','vendor/gfw_eval.c')
 foreach($test in @('efi_transaction','guards','prepare4070s','rebar4070s')) {
  $exe="build/core4070s/test-$test.exe"
  $compileArgs=$common+$includes+@("tests/test_$test.c")+$sources+@('-o',$exe)
  $commands.Add([ordered]@{stage='compile';executable=$Gcc;arguments=$compileArgs})
  & $Gcc @compileArgs *> (Join-Path $Evidence "compile-$test.log")
  if($LASTEXITCODE -ne 0){Get-Content (Join-Path $Evidence "compile-$test.log");throw "Compile failure (not semantic RED): $test"}
 }
 $cases=@(
  @{name='prepare-pins';test='prepare4070s';arguments=@($Prom,$Assets,'pins')},
  @{name='prepare-positive';test='prepare4070s';arguments=@($Prom,$Assets,'positive')},
  @{name='prepare-rejections';test='prepare4070s';arguments=@($Prom,$Assets,'rejections')},
  @{name='guards';test='guards';arguments=@($Prom,$Assets)},
  @{name='efi-transaction';test='efi_transaction';arguments=@($Prom,$Assets)}
 )
 foreach($case in @('save','save-read-errors','restore','restore-size-bit13','restore-faults','restore-structure','read-boundary','write-boundary')) {
  $cases+=@{name="rebar-$case";test='rebar4070s';arguments=@($case)}
 }
 $cases+=@{name='rebar-oracle-transient-bme';test='rebar4070s';arguments=@('oracle-transient-bme');expected_exit=1;expected_assertion='!(written_command_low&4)'}
 if($Groups){foreach($group in $Groups){if($group -notin $cases.name){throw "Unknown test group: $group"}}}
 if(!$CompileOnly) {
  foreach($case in $cases) {
   if($Groups -and $case.name -notin $Groups){continue}
   $exe=Join-Path $PSScriptRoot ("build/core4070s/test-"+$case.test+'.exe')
   $runArgs=$case.arguments
   $commands.Add([ordered]@{stage='test';group=$case.name;executable=$exe;arguments=$runArgs})
   $log=Join-Path $Evidence ($case.name+'.log')
   $ErrorActionPreference='Continue'
   & $exe @runArgs *> $log
   $code=$LASTEXITCODE
   $ErrorActionPreference='Stop'
   $expectedExit=if($case.ContainsKey('expected_exit')){$case.expected_exit}else{0}
   $observed=$code -eq $expectedExit
   if($case.ContainsKey('expected_assertion')) {
    $observed=$observed -and (Get-Content -Raw -LiteralPath $log).Contains($case.expected_assertion)
   }
   $results.Add([ordered]@{group=$case.name;compiled=$true;exit_code=$code;expected_exit_code=$expectedExit;expected_outcome_observed=$observed;log=$log})
   Write-Output ($case.name+': exit '+$code+' expected '+$expectedExit)
   Get-Content -LiteralPath $log -Tail 3
  }
 }
 $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Evidence 'test-results.json') -Encoding utf8
 $commands | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Evidence 'commands.json') -Encoding utf8
 $failed=@($results | Where-Object {!$_.expected_outcome_observed})
 if($failed.Count){throw "$($failed.Count) compiled behavior groups failed; see $Evidence"}
 Write-Output '4070S core tests completed. PCI/MMIO/DMA/firmware simulated; no hardware qualification.'
} finally {
 $env:PATH=$oldPath
 $process.PriorityClass=$oldPriority
 Pop-Location
}
