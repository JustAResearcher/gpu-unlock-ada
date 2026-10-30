<#
.SYNOPSIS
    Configures Windows 11 / 10 Display Class Registry to disable NVIDIA RTD3 (D3cold)
    runtime power transitions on Hybrid / Optimus laptops, preventing in-RAM tensor
    unlock registers (SS0/SS1) from resetting to stock.

.DESCRIPTION
    Based on the groundbreaking investigation by Mayhem50 in upstream issue:
    https://github.com/JustAResearcher/gpu-unlock-ada/issues/117

    On laptops running in MSHybrid / Optimus mode (where internal display is driven
    by Intel/AMD iGPU), Windows initiates an NVIDIA D3cold transition ~563ms after
    driver start. Waking from D3cold clears volatile MMIO state (SS0=0x04003025).
    Setting EnableRuntimePowerManagement=0 stops D3cold without breaking display
    output or brightness control.
#>

param(
    [switch]$Restore,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   WINDOWS HYBRID / OPTIMUS LAPTOP D3cold FIX FOR GPU UNLOCK          " -ForegroundColor White
Write-Host "   Research & Discovery: Mayhem50 (upstream Issue #117)               " -ForegroundColor Yellow
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host ""

# 1. Check Administrator privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[ERROR] This script requires Administrator privileges!" -ForegroundColor Red
    Write-Host "Please right-click the batch file and select 'Run as Administrator'." -ForegroundColor Yellow
    exit 1
}

$ClassGuid = "{4d36e968-e325-11ce-bfc1-08002be10318}"
$BasePath = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"

if (-not (Test-Path $BasePath)) {
    Write-Host "[ERROR] Display adapter class registry key not found: $BasePath" -ForegroundColor Red
    exit 1
}

$subkeys = Get-ChildItem -Path $BasePath | Where-Object { $_.PSChildName -match '^\d{4}$' }
$foundCount = 0

foreach ($sub in $subkeys) {
    $prop = Get-ItemProperty -Path $sub.PSPath -ErrorAction SilentlyContinue
    if (-not $prop) { continue }

    $isNvidia = $false
    if ($prop.ProviderName -and $prop.ProviderName -match "NVIDIA") { $isNvidia = $true }
    if ($prop.MatchingDeviceId -and $prop.MatchingDeviceId -match "VEN_10DE") { $isNvidia = $true }
    if ($prop.DriverDesc -and $prop.DriverDesc -match "NVIDIA") { $isNvidia = $true }

    if ($isNvidia) {
        $foundCount++
        $keyName = $sub.PSChildName
        $desc = if ($prop.DriverDesc) { $prop.DriverDesc } else { "NVIDIA Graphics Adapter" }
        Write-Host "[+] Found NVIDIA Adapter: [$keyName] $desc" -ForegroundColor Green

        $currentVal = $null
        if ($null -ne (Get-ItemProperty -Path $sub.PSPath -Name "EnableRuntimePowerManagement" -ErrorAction SilentlyContinue)) {
            $currentVal = (Get-ItemProperty -Path $sub.PSPath -Name "EnableRuntimePowerManagement").EnableRuntimePowerManagement
        }

        if ($CheckOnly) {
            if ($null -eq $currentVal) {
                Write-Host "    Status: Default (EnableRuntimePowerManagement absent -> D3cold enabled)" -ForegroundColor Gray
            } elseif ($currentVal -eq 0) {
                Write-Host "    Status: ALREADY PATCHED (EnableRuntimePowerManagement=0 -> D3cold disabled)" -ForegroundColor Green
            } else {
                Write-Host "    Status: Value=$currentVal (D3cold enabled)" -ForegroundColor Yellow
            }
            continue
        }

        if ($Restore) {
            if ($null -ne $currentVal) {
                Remove-ItemProperty -Path $sub.PSPath -Name "EnableRuntimePowerManagement" -Force -ErrorAction SilentlyContinue
                Write-Host "    [RESTORED] Removed EnableRuntimePowerManagement (reverted to system default)" -ForegroundColor Yellow
            } else {
                Write-Host "    [INFO] Key was already at default state." -ForegroundColor Gray
            }
        } else {
            Set-ItemProperty -Path $sub.PSPath -Name "EnableRuntimePowerManagement" -Value 0 -Type DWord -Force
            $verify = (Get-ItemProperty -Path $sub.PSPath -Name "EnableRuntimePowerManagement").EnableRuntimePowerManagement
            if ($verify -eq 0) {
                Write-Host "    [SUCCESS] Set EnableRuntimePowerManagement=0 (DWORD)" -ForegroundColor Green
                Write-Host "              NVIDIA D3cold power transition disabled." -ForegroundColor Green
            } else {
                Write-Host "    [FAIL] Failed to verify value write." -ForegroundColor Red
            }
        }
    }
}

if ($foundCount -eq 0) {
    Write-Host "[!] No NVIDIA adapters detected under $ClassGuid!" -ForegroundColor Yellow
} else {
    Write-Host ""
    if (-not $CheckOnly -and -not $Restore) {
        Write-Host "======================================================================" -ForegroundColor Cyan
        Write-Host "[OK] Configuration applied successfully for $foundCount adapter(s)!" -ForegroundColor Green
        Write-Host "     Your laptop will now preserve in-RAM UEFI tensor unlock across" -ForegroundColor White
        Write-Host "     Windows driver start in MSHybrid / Optimus mode." -ForegroundColor White
        Write-Host "======================================================================" -ForegroundColor Cyan
    }
}
