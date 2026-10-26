param([string]$Gcc='gcc',[string]$Python='python',[string]$VcVars64)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 & .\Build.ps1 -Gcc $Gcc -Python $Python
 if(!$?){throw 'Core build failed'}
 $flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_AD104_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-Icompat','-Ivendor/edk2/Include','-Ivendor/edk2/Include/X64','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-mno-red-zone','-fshort-wchar','-fstack-usage')
 foreach($s in @('app','boot_io','msi_release')) {
  & $Gcc @flags -c ($s+'.c') -o ('build/'+$s+'.o')
  if($LASTEXITCODE -ne 0){throw "Compile failed: $s"}
 }
 $vcvars=$VcVars64
 if(!$vcvars){
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if(!(Test-Path -LiteralPath $vswhere)){throw 'Pass -VcVars64 with the MSVC x64 setup script'}
  $vcvars=@(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvars64.bat') | Select-Object -First 1
 }
 if(!$vcvars -or !(Test-Path -LiteralPath $vcvars)){throw 'MSVC x64 tools not found'}
 if($vcvars -match '["&|<>%^!\r\n]'){throw 'Unsupported command-shell path characters'}
 if($PSScriptRoot -match '["&|<>%^!\r\n]'){throw 'Unsupported command-shell path characters'}
 $objects=@('app','boot_io','msi_release','port','compat','checks','profile','sha256','runtime','format','vendor_unlock_610','vendor_prep_610','vendor_layout_610','vendor_vbios_610','vendor_gfw_eval')
 $lines=@('@echo off',('call "'+$vcvars+'" >nul'),'if errorlevel 1 exit /b 1',('cd /d "'+$PSScriptRoot+'"'),('link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /brepro /out:build\AD104-EXPERIMENTAL.EFI '+(($objects | ForEach-Object {'build\'+$_+'.o'}) -join ' ')),'exit /b %errorlevel%')
 Set-Content -LiteralPath build/link-msvc.cmd -Value $lines -Encoding ascii
 & $env:ComSpec /d /c ('"'+(Join-Path $PSScriptRoot 'build\link-msvc.cmd')+'"')
 if($LASTEXITCODE -ne 0){throw 'EFI link failed'}
 & $Python tools/verify_experimental.py
 if($LASTEXITCODE -ne 0){throw 'EFI verification failed'}
 $hashes=[ordered]@{}
 Get-ChildItem -LiteralPath . -Recurse -File | Where-Object {$_.Extension -in @('.c','.h','.ps1','.py')} | Sort-Object FullName | ForEach-Object {
  $relative=$_.FullName.Substring($PSScriptRoot.Length+1)
  if($relative -notmatch '^(build[^\\]*|work)\\'){$hashes[$relative]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
 }
 [ordered]@{version='0.6.0-4070s-manual-trial';mode='manual-experimental-one-physical-attempt';binary='AD104-EXPERIMENTAL.EFI';sha256=(Get-FileHash build/AD104-EXPERIMENTAL.EFI).Hash.ToLowerInvariant();gpu='10de:2783 subsystem 1458:413b';motherboard='MSI MAG Z790 TOMAHAWK WIFI H.E0';gpu_write_capability=$true;hardware_tested=$false;previous_hardware_run='2026-10-25: one successful manual unlock, Windows driver handoff and matched GEMM measurements';tested_binary_sha256='b20394d76f7b7dc706e7d7ba893a4ca56b958db66232b7b22ed60a63d28acf84';boot_source='pinned existing internal ESP';gsp_source='pinned C partition via native EFI SimpleFileSystem; no fallback';native_ntfs_verified_in_previous_run=$true;automatic_unlock=$false;requires_key='U';default='stock Windows after 30s if preflight passes';source_hashes=$hashes;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content build/experimental-manifest.json -Encoding utf8
} finally {Pop-Location}
