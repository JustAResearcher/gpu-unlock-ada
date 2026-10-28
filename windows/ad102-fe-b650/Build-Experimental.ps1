param([string]$Gcc='gcc',[string]$Python='python',[string]$VcVars64)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
 & .\Build.ps1 -Gcc $Gcc -Python $Python
 if(!$?){throw 'Core build failed'}
 $flags=@('-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-D__KERNEL__','-DGU_UEFI_PORT','-DGU_PROFILE_TRANSACTION','-DGU_ENTRY_LATCHED_SS1_FULL','-Icompat','-I../ad104-msi/vendor/edk2/Include','-I../ad104-msi/vendor/edk2/Include/X64','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-mno-red-zone','-fshort-wchar','-fstack-usage')
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
 $lines=@('@echo off',('call "'+$vcvars+'" >nul'),'if errorlevel 1 exit /b 1',('cd /d "'+$PSScriptRoot+'"'),('link /nologo /subsystem:efi_application /entry:UefiMain /machine:x64 /nodefaultlib /incremental:no /fixed:no /dynamicbase:no /nxcompat:no /brepro /out:build\AD102-FE-EXPERIMENTAL.EFI '+(($objects | ForEach-Object {'build\'+$_+'.o'}) -join ' ')),'exit /b %errorlevel%')
 Set-Content -LiteralPath build/link-msvc.cmd -Value $lines -Encoding ascii
 & $env:ComSpec /d /c ('"'+(Join-Path $PSScriptRoot 'build\link-msvc.cmd')+'"')
 if($LASTEXITCODE -ne 0){throw 'EFI link failed'}
 & $Python tools/verify_experimental.py
 if($LASTEXITCODE -ne 0){throw 'EFI verification failed'}
 $hashes=[ordered]@{}
 Get-ChildItem -LiteralPath . -Recurse -File | Where-Object {$_.Extension -in @('.c','.h','.ps1','.py') -and $_.FullName -notlike '*\build\*'} | Sort-Object FullName | ForEach-Object {$hashes[$_.FullName.Substring($PSScriptRoot.Length+1)]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
 [ordered]@{version='0.1.0';mode='manual-experimental-one-physical-attempt';binary='AD102-FE-EXPERIMENTAL.EFI';sha256=(Get-FileHash build/AD102-FE-EXPERIMENTAL.EFI).Hash.ToLowerInvariant();gpu_write_capability=$true;hardware_tested=$false;previous_hardware_run='none on AD102 FE / MSI B650; release rules inherit AD104 v0.5.0-v0.5.3 runs';automatic_unlock=$false;requires_key='U';default='stock Windows after 30s';source_hashes=$hashes;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content build/experimental-manifest.json -Encoding utf8
} finally {Pop-Location}
