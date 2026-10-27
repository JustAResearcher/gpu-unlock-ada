param(
 [string]$Gcc='gcc',
 [string]$Ar='ar',
 [string]$Ld='ld',
 [string]$Python='python',
 [string]$VcVars64,
 [switch]$UseLd
)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 & .\Build.ps1 -Gcc $Gcc -Ar $Ar -Python $Python
 if(!$?){throw 'Core build failed'}
 $flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-Icompat','-Ivendor/edk2/Include','-Ivendor/edk2/Include/X64','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-mno-red-zone','-fshort-wchar','-fstack-usage')
 foreach($s in @('app','boot_io','x99_release')) {
  & $Gcc @flags -c ($s+'.c') -o ('build/'+$s+'.o')
  if($LASTEXITCODE -ne 0){throw "Compile failed: $s"}
 }
 $objects=@('app','boot_io','x99_release','port','compat','checks','profile','sha256','runtime','format','vendor_unlock_610','vendor_prep_610','vendor_layout_610','vendor_vbios_610','vendor_gfw_eval')
 $objFiles=@($objects | ForEach-Object {'build/'+$_+'.o'})

 $vcvars=$VcVars64
 if(!$vcvars -and !$UseLd){
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if(Test-Path -LiteralPath $vswhere){
   $vcvars=@(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat') | Select-Object -First 1
  }
 }

 if($vcvars -and (Test-Path -LiteralPath $vcvars) -and !$UseLd){
  Write-Host 'Linking EFI with MSVC link...'
  $lines=@('@echo off',('call "'+$vcvars+'" >nul'),'if errorlevel 1 exit /b 1',('cd /d "'+$PSScriptRoot+'"'),('link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /brepro /out:build\AD104-EXPERIMENTAL.EFI '+($objFiles -join ' ')),'exit /b %errorlevel%')
  Set-Content -LiteralPath build/link-msvc.cmd -Value $lines -Encoding ascii
  & $env:ComSpec /d /c ('"'+(Join-Path $PSScriptRoot 'build\link-msvc.cmd')+'"')
  if($LASTEXITCODE -ne 0){throw 'EFI link failed'}
 } else {
  Write-Host 'Linking EFI with GNU ld...'
  $targetLd = $Ld
  if($targetLd -eq 'ld') {
   $whichLd = Get-Command 'x86_64-w64-mingw32-ld.exe' -ErrorAction SilentlyContinue
   if($whichLd){$targetLd = $whichLd.Source}
  }
  & $targetLd -shared -nostdlib --subsystem 10 --image-base 0x180000000 -e UefiMain -o build/AD104-EXPERIMENTAL.EFI @objFiles
  if($LASTEXITCODE -ne 0){throw 'GNU ld EFI link failed'}
  # Zero out imports directory entry slot 1 in PE header
  & $Python -c "import struct; from pathlib import Path; p = Path('build/AD104-EXPERIMENTAL.EFI'); d = bytearray(p.read_bytes()); pe = struct.unpack_from('<I', d, 60)[0]; opt = pe + 24; d[opt + 112 + 8 : opt + 112 + 16] = b'\x00' * 8; p.write_bytes(d)"
 }

 & $Python tools/verify_experimental.py
 if($LASTEXITCODE -ne 0){throw 'EFI verification failed'}
 $hashes=[ordered]@{}
 Get-ChildItem -LiteralPath . -Recurse -File | Where-Object {$_.Extension -in @('.c','.h','.ps1','.py') -and $_.FullName -notlike '*\build\*'} | Sort-Object FullName | ForEach-Object {$hashes[$_.FullName.Substring($PSScriptRoot.Length+1)]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
 [ordered]@{version='0.5.4';mode='manual-experimental-one-physical-attempt';binary='AD104-EXPERIMENTAL.EFI';sha256=(Get-FileHash build/AD104-EXPERIMENTAL.EFI).Hash.ToLowerInvariant();gpu_write_capability=$true;hardware_tested=$true;previous_hardware_run='v0.5.4: hardware unlock validated on Huananzhi X99-F8D Plus + Gigabyte RTX 4070 Ti (10de:2782/1458:40e8). Driver 616.92 booted without error. BF16 GEMM 143.76 TFLOPS at 285W';automatic_unlock=$false;requires_key='U';default='stock Windows after 30s';source_hashes=$hashes;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content build/experimental-manifest.json -Encoding utf8
} finally {Pop-Location}

