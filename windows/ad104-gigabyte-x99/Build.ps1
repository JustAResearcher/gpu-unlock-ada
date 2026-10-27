param(
 [string]$Gcc='gcc',
 [string]$Ar='ar',
 [string]$Python='python',
 [string]$Prom,
 [string]$Assets,
 [switch]$RunTests
)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 New-Item -ItemType Directory -Path build -Force | Out-Null
 $common=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-Icompat','-Ivendor/edk2/Include','-Ivendor/edk2/Include/X64')
 $sources=@('port','compat','checks','profile','sha256','runtime','format','vendor/unlock_610','vendor/prep_610','vendor/layout_610','vendor/vbios_610','vendor/gfw_eval')
 $objects=@()
 foreach($s in $sources) {
  $o='build/'+($s -replace '/','_')+'.o'
  & $Gcc @common -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -mno-red-zone -fshort-wchar -fstack-usage -c ($s+'.c') -o $o
  if($LASTEXITCODE -ne 0){throw "Freestanding compile failed: $s"};$objects+=$o
 }
 $archive='build/AD104-TRANSACTION.a'
 # Replace only this archive: ar otherwise keeps obsolete members between builds.
 if(Test-Path -LiteralPath $archive){Remove-Item -LiteralPath $archive}
 & $Ar rcs $archive @objects
 if($LASTEXITCODE -ne 0){throw 'Archive failed'}
 & $Python tools/verify_archive.py
 if($LASTEXITCODE -ne 0){throw 'Archive verification failed'}
 if($RunTests) {
  if(!(Test-Path -LiteralPath $Prom -PathType Leaf) -or !(Test-Path -LiteralPath $Assets -PathType Container)){throw 'Tests require the private captured PROM and generated Ada assets'}
  $testSources=@($sources | Where-Object {$_ -notin @('runtime','format')} | ForEach-Object {$_+'.c'})
  foreach($test in @('guards','efi_transaction')) {
   & $Gcc @common -D_CRT_SECURE_NO_WARNINGS ("tests/test_$test.c") @testSources -o ("build/test-$test.exe")
   if($LASTEXITCODE -ne 0){throw "Test compile failed: $test"}
   & (".\build\test-$test.exe") $Prom $Assets > ("build/$test.log") 2>&1
   if($LASTEXITCODE -ne 0){Get-Content ("build/$test.log") -Tail 35;throw "Test failure: $test"}
   Get-Content ("build/$test.log") -Tail 1
  }
 }
 $pins=[ordered]@{}
 Get-ChildItem -LiteralPath . -Recurse -File | Where-Object {$_.Extension -in @('.c','.h','.ps1','.py') -and $_.FullName -notlike '*\build\*'} | Sort-Object FullName | ForEach-Object {$pins[$_.FullName.Substring($PSScriptRoot.Length+1)]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
 [ordered]@{artifact='AD104-TRANSACTION.a';sha256=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant();source_hashes=$pins;tests_run=[bool]$RunTests;bootable=$false;hardware_unlock_validated=$false;gpu_writes_performed=$false;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath build/manifest.json -Encoding utf8
 Write-Output 'Built AD104 freestanding x64 transaction library. No EFI boot entry and no GPU execution.'
} finally {Pop-Location}
