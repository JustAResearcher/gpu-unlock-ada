# SPDX-License-Identifier: GPL-2.0-only
# Adapted from ../ad104-msi/Build.ps1 -- same flags/source list/logic, this
# board's own local files (port.c/profile.c/profile.h/port.h are this
# board's own; compat/checks/sha256/runtime/format/vendor/* are generic,
# copied byte-identical from ad104-msi, confirmed by cmp when copied).
# Builds AD103-TRANSACTION.a: freestanding x64 object library, no EFI boot
# entry (app.c/boot_io.c/z790_release.c are not in this archive -- see
# Build-Experimental.ps1), no GPU execution.
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
 $common=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-Icompat','-I../ad104-msi/vendor/edk2/Include','-I../ad104-msi/vendor/edk2/Include/X64')
 $sources=@('port','compat','checks','profile','sha256','runtime','format','vendor/unlock_610','vendor/prep_610','vendor/layout_610','vendor/vbios_610','vendor/gfw_eval')
 $objects=@()
 foreach($s in $sources) {
  $o='build/'+($s -replace '/','_')+'.o'
  & $Gcc @common -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -mno-red-zone -fshort-wchar -fstack-usage -c ($s+'.c') -o $o
  if($LASTEXITCODE -ne 0){throw "Freestanding compile failed: $s"};$objects+=$o
 }
 $archive='build/AD103-TRANSACTION.a'
 if(Test-Path -LiteralPath $archive){Remove-Item -LiteralPath $archive}
 & $Ar rcs $archive @objects
 if($LASTEXITCODE -ne 0){throw 'Archive failed'}
 & $Python tools/verify_archive.py
 if($LASTEXITCODE -ne 0){throw 'Archive verification failed'}
 if($RunTests) {
  if(!(Test-Path -LiteralPath $Prom -PathType Leaf) -or !(Test-Path -LiteralPath $Assets -PathType Container)){throw 'Tests need -Prom (a 1 MiB image of this card''s VBIOS) and -Assets (the pinned generated Ada assets)'}
  # The pinned PROM hash is the live pre-driver BAR0 capture. Host tests run on
  # whatever 1 MiB VBIOS image -Prom points at, so they pin that file's own hash.
  # Its FWSEC region must still match the pinned geometry (checked by the tests).
  $promHash=(Get-FileHash -LiteralPath $Prom -Algorithm SHA256).Hash.ToLowerInvariant()
  Set-Content -LiteralPath build/test_prom_hash.h -Value ('#define GU_HOST_TEST_PROM_SHA256 "'+$promHash+'"') -Encoding ascii
  $testSources=@($sources | Where-Object {$_ -notin @('runtime','format')} | ForEach-Object {$_+'.c'})
  foreach($test in @('guards','efi_transaction')) {
   & $Gcc @common -include build/test_prom_hash.h -D_CRT_SECURE_NO_WARNINGS ("tests/test_$test.c") @testSources -o ("build/test-$test.exe")
   if($LASTEXITCODE -ne 0){throw "Test compile failed: $test"}
   & (".\build\test-$test.exe") $Prom $Assets > ("build/$test.log") 2>&1
   if($LASTEXITCODE -ne 0){Get-Content ("build/$test.log") -Tail 35;throw "Test failure: $test"}
   Get-Content ("build/$test.log") -Tail 1
  }
 }
 $pins=[ordered]@{}
 Get-ChildItem -LiteralPath . -Recurse -File | Where-Object {$_.Extension -in @('.c','.h','.ps1','.py') -and $_.FullName -notlike '*\build\*' -and $_.FullName -notlike '*\local\*'} | Sort-Object FullName | ForEach-Object {$pins[$_.FullName.Substring($PSScriptRoot.Length+1)]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
 [ordered]@{artifact='AD103-TRANSACTION.a';sha256=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant();source_hashes=$pins;tests_run=[bool]$RunTests;bootable=$false;hardware_unlock_validated=$false;gpu_writes_performed=$false;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath build/manifest.json -Encoding utf8
 Write-Output 'Built AD103 freestanding x64 transaction library. No EFI boot entry and no GPU execution.'
} finally {Pop-Location}
