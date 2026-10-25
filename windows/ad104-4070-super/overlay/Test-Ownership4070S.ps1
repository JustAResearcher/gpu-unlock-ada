param(
 [string]$Gcc='gcc',
 [Parameter(Mandatory=$true)][string]$DriverCode,
 [string]$Evidence,
 [string[]]$Groups,
 [switch]$CompileOnly
)
$ErrorActionPreference='Stop'
if(!$Evidence){$Evidence=Join-Path $PSScriptRoot 'build/test-evidence/ownership'}
$Evidence=[System.IO.Path]::GetFullPath($Evidence)
$DriverCode=(Resolve-Path -LiteralPath $DriverCode).Path
# Frozen C fixtures carry the reviewed graph and image metadata; only code bytes are external inputs.
$expected=@(
 @{agent=310;bytes=20384;sha='0febfc8415226067e906adecb896ab45b03e969eac360a01ccfe81dd04c8a47e'},
 @{agent=315;bytes=7712;sha='ece28d4e20f28f5ca16bbe0a49ad6b0ad33956d55851c4ed620f617a6c237097'},
 @{agent=406;bytes=138183;sha='216c7270fa08ad9e4c3694d34d825e9e904554a1af5ffa2aeead4317c5537b37'}
)
foreach($pin in $expected) {
 $file=Join-Path $DriverCode ('driver-'+$pin.agent+'-code.bin')
 if((Get-Item -LiteralPath $file).Length -ne $pin.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.sha) {
  throw "Fixture-input failure (not semantic RED): driver $($pin.agent) length/SHA256."
 }
}
$known=@('fixtures','capture','driver-pins','pin-rejections','legacy','guards','handoff')
if(!$Groups){$Groups=$known}
foreach($group in $Groups){if($group -notin $known){throw "Unknown ownership test group: $group"}}
$process=[System.Diagnostics.Process]::GetCurrentProcess()
$oldPriority=$process.PriorityClass
$oldPath=$env:PATH
$commands=[System.Collections.Generic.List[object]]::new()
$results=[System.Collections.Generic.List[object]]::new()
Push-Location -LiteralPath $PSScriptRoot
try {
 $process.PriorityClass='BelowNormal'
 $env:PATH=(Split-Path -Parent (Get-Command $Gcc -CommandType Application).Source)+';'+$env:PATH
 New-Item -ItemType Directory -Path 'build-ownership4070s' -Force | Out-Null
 New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
 $Evidence=(Resolve-Path -LiteralPath $Evidence).Path
 $flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
  '-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-D_CRT_SECURE_NO_WARNINGS',
  '-Icompat','-Ivendor/edk2/Include','-Ivendor/edk2/Include/X64')
 $source=Join-Path $PSScriptRoot 'msi_release.c'
 $exe=Join-Path $PSScriptRoot 'build-ownership4070s/test-msi-release-current.exe'
 $compileArgs=$flags+@('tests/test_msi_release.c','sha256.c','format.c','-o',$exe)
 $commands.Add([ordered]@{stage='compile';source='current';executable=$Gcc;arguments=$compileArgs})
 $compileLog=Join-Path $Evidence 'compile-current.log'
 & $Gcc @compileArgs *> $compileLog
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $compileLog;throw 'Compile failure (not semantic RED): current'}
 if(!$CompileOnly) {
  foreach($group in $Groups) {
   $log=Join-Path $Evidence ('current-'+$group+'.log')
   $commands.Add([ordered]@{stage='test';source='current';group=$group;executable=$exe;arguments=@($DriverCode,$group)})
   $ErrorActionPreference='Continue'
   & $exe $DriverCode $group *> $log
   $code=$LASTEXITCODE
   $ErrorActionPreference='Stop'
   $results.Add([ordered]@{source='current';group=$group;compiled=$true;exit_code=$code;
    source_sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant();log=$log})
   Write-Output ('current/'+$group+': exit '+$code)
   Get-Content -LiteralPath $log -Tail 4
  }
 }
 $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Evidence 'test-results.json') -Encoding utf8
 $commands | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Evidence 'commands.json') -Encoding utf8
 $failed=@($results | Where-Object {$_.exit_code -ne 0})
 if($failed.Count){throw "$($failed.Count) compiled behavior groups failed; see $Evidence"}
 Write-Output '4070S ownership host tests completed. Captured code never executed; no hardware/driver Stop qualification.'
} finally {
 $env:PATH=$oldPath
 $process.PriorityClass=$oldPriority
 Pop-Location
}
