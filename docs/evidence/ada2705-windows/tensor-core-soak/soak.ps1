param([int]$Seconds = 300, [string]$Label = "unlocked")
# Sustained load: loops their mma_bench + cublas_gemm for $Seconds while sampling power/clock/temp.
Set-Location $PSScriptRoot
$env:Path = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.5\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.5\bin\x64;" + $env:Path
$out = "D:\AI\bench\soak_$Label"
New-Item -ItemType Directory -Force $out | Out-Null
$smi = Start-Process nvidia-smi -ArgumentList '--query-gpu=timestamp,power.draw,clocks.sm,temperature.gpu,utilization.gpu,clocks_throttle_reasons.active --format=csv,noheader,nounits -lms 500' -RedirectStandardOutput "$out\smi.csv" -PassThru -WindowStyle Hidden
$end = (Get-Date).AddSeconds($Seconds); $i = 0
while ((Get-Date) -lt $end) {
    $i++
    ".\mma_bench.exe 20000 5" | Out-File -Append "$out\runs.txt"; & .\mma_bench.exe 20000 5 | Out-File -Append "$out\runs.txt"
    if ((Get-Date) -ge $end) { break }
    "cublas_gemm 8192 40 3" | Out-File -Append "$out\runs.txt"; & .\cublas_gemm.exe 8192 40 3 | Out-File -Append "$out\runs.txt"
}
Start-Sleep 1; Stop-Process $smi.Id -ErrorAction SilentlyContinue
$r = Get-Content "$out\smi.csv" | % { $f = $_ -split ',\s*'; [pscustomobject]@{w=[double]$f[1]; mhz=[int]$f[2]; t=[int]$f[3]; u=[int]$f[4]; thr=$f[5]} }
$busy = $r | ? { $_.u -gt 90 }
$sum = "label=$Label seconds=$Seconds loops=$i samples=$($r.Count) busy=$($busy.Count)`n" +
  "power W: max=$(($r|measure w -Max).Maximum) avg(busy)=$([math]::Round(($busy|measure w -Average).Average,1))`n" +
  "sm MHz: min(busy)=$(($busy|measure mhz -Minimum).Minimum) avg(busy)=$([math]::Round(($busy|measure mhz -Average).Average))  max=$(($r|measure mhz -Max).Maximum)`n" +
  "temp C: max=$(($r|measure t -Max).Maximum)`n" +
  (($r | group thr | % { "throttle $($_.Name): $($_.Count)" }) -join "`n")
$sum | Out-File "$out\summary.txt"
"DONE" | Out-File "$out\DONE"
