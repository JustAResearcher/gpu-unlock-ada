# SPDX-License-Identifier: GPL-2.0-only
# Parse the real Windows launcher, then exercise only its post-platform-guard
# statements with mocked paths/interpreters. No Python or CUDA is executed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$script:Checks = 0
function Assert([bool]$Condition,[string]$Message) {
    if (-not $Condition) {throw $Message};$script:Checks++
}
$path = Join-Path $PSScriptRoot '../usb-staged/Run-Benchmark.ps1'
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$parseErrors)
Assert ($parseErrors.Count -eq 0) 'Launcher does not parse.'
$guards=@($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and
    $_.Extent.Text.Contains('[Environment]::OSVersion.Platform')
})
Assert ($guards.Count -eq 1 -and $guards[0].Extent.Text.Contains('[PlatformID]::Win32NT') -and
    $guards[0].Extent.Text.Contains('throw')) 'Production Windows-only guard was removed or changed.'
Assert ($ast.ParamBlock.Extent.Text.Contains("ValidateSet('stock','unlocked','compare')")) 'Mode validation is absent.'
Assert ($ast.ParamBlock.Extent.Text.Contains('[string]$GpuUuid')) 'Explicit UUID parameter is absent.'
$commands=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst]},$true) |
    ForEach-Object {$_.GetCommandName()} | Where-Object {$_})
foreach ($forbidden in @('Set-ExecutionPolicy','Install-Module','Install-Package','Start-Process',
    'Invoke-WebRequest','Invoke-RestMethod','Set-ItemProperty','Suspend-BitLocker','Resume-BitLocker',
    'winget','pip','bcdedit','pnputil')) {
    Assert ($commands -notcontains $forbidden) ('Launcher contains a settings/install command: '+$forbidden)
}
# Tests never bypass this guard in the distributed file. The copied AST suffix
# has only process-launch logic; all external operations below are mocks.
$script:FixtureRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../usb-staged'))
$body=$ast.Extent.Text.Substring($guards[0].Extent.EndOffset).Replace('$PSScriptRoot','$FixtureRoot')
$script:Program=[ScriptBlock]::Create('param([string]$FixtureRoot)' + [Environment]::NewLine + $body)
function Run-Fixture([string]$Mode,[string]$Case='',[string]$Selection='') {
    $state=[pscustomobject]@{Case=$Case;Calls=(New-Object 'System.Collections.Generic.List[object]');Prompts=0;Error=$null;Output=@()}
    & {
        param($State,$Mode,$Program,$GpuUuid)
        function Read-Host {
            param($Prompt)
            $State.Prompts++
            switch ($State.Case) {
                'empty-uuid' {return ''}
                'index-uuid' {return '0'}
                'partial-uuid' {return 'GPU-11111111'}
                'newline-uuid' {return "GPU-11111111-2222-3333-4444-555555555555`n"}
            }
            return 'GPU-11111111-2222-3333-4444-555555555555'
        }
        function Test-Path {param($LiteralPath,$PathType) return $State.Case -ne 'missing-runner'}
        function Get-Command {
            param($Name,$CommandType,$ErrorAction)
            if ($State.Case -eq 'missing-python') {return $null}
            if ($State.Case -eq 'store-alias') {
                if ($Name -eq 'py.exe') {return $null}
                if ($Name -eq 'python.exe') {return [pscustomobject]@{Source='C:\Users\test\AppData\Local\Microsoft\WindowsApps\python.exe'}}
            }
            return [pscustomobject]@{Source=$(if($Name -eq 'py.exe') {'Invoke-FakeLauncher'} else {'Invoke-FakePlain'})}
        }
        function Invoke-FakeInterpreter([string]$Kind,[object[]]$Arguments) {
            $State.Calls.Add([pscustomobject]@{Kind=$Kind;Arguments=$Arguments})
            $global:LASTEXITCODE=0
            if ($Arguments -contains '-c') {
                $probe=[string]$Arguments[-1]
                Assert (-not $probe.Contains('"')) 'Python probe contains legacy-marshalling-sensitive double quotes.'
                Assert ($Arguments -contains '-I') 'Interpreter probe omitted isolated mode.'
                if ($Kind -eq 'launcher') {
                    switch ($State.Case) {
                        'probe-failed' {$global:LASTEXITCODE=7;return 'GU_PYTHON_OK'}
                        'probe-unsupported' {return 'UNSUPPORTED'}
                        'probe-noisy' {return @('unexpected output','GU_PYTHON_OK')}
                    }
                }
                return 'GU_PYTHON_OK'
            }
            if ($State.Case -eq 'run-failed') {$global:LASTEXITCODE=17}
            return 'mock benchmark output'
        }
        function Invoke-FakeLauncher {Invoke-FakeInterpreter 'launcher' @($args)}
        function Invoke-FakePlain {Invoke-FakeInterpreter 'plain' @($args)}
        function Get-ChildItem {
            param($LiteralPath,$Filter,[switch]$File,$ErrorAction)
            if ($State.Case -eq 'missing-results') {return @()}
            $label=if($Filter -eq 'stock-*.json'){'stock'}else{'unlocked'}
            return @([pscustomobject]@{FullName=(Join-Path $LiteralPath ($label+'-older.json'));LastWriteTimeUtc=[DateTime]'2026-01-01'},
                     [pscustomobject]@{FullName=(Join-Path $LiteralPath ($label+'-newest.json'));LastWriteTimeUtc=[DateTime]'2026-02-01'})
        }
        try {$State.Output=@(& $Program -FixtureRoot $script:FixtureRoot)} catch {$State.Error=$_.Exception.Message}
    } $state $Mode $script:Program $Selection
    return $state
}
foreach ($mode in @('stock','unlocked')) {
    $run=Run-Fixture $mode
    Assert ($null -eq $run.Error) ('Successful '+$mode+' launch failed: '+$run.Error)
    Assert ($run.Calls.Count -eq 2) 'Unexpected interpreter executions.'
    $args=@($run.Calls[1].Arguments)
    Assert ($run.Calls[1].Kind -eq 'launcher' -and $args[0] -ceq '-3' -and $args[1] -ceq '-I') 'Launcher prefix/isolated mode missing.'
    Assert ($args[2] -match 'benchmark[\\/]run_benchmark\.py$') 'Wrong benchmark script.'
    Assert (($args[3..5] -join ',') -ceq ('run,--label,'+$mode)) 'Boot-session label not forwarded.'
    Assert ($run.Prompts -eq 1 -and $args[6] -ceq '--gpu-uuid' -and $args[7] -ceq 'GPU-11111111-2222-3333-4444-555555555555') 'Explicit prompted UUID not forwarded.'
    Assert ($args[8] -ceq '--output-dir' -and $args[9] -match 'benchmark-results$') 'Wrong results directory.'
}
foreach ($case in @('empty-uuid','index-uuid','partial-uuid','newline-uuid')) {
    $run=Run-Fixture 'stock' $case
    Assert ($null -ne $run.Error -and $run.Error.Contains('full GPU-... UUID is required')) ('Invalid UUID accepted: '+$case)
    Assert ($run.Calls.Count -eq 0) 'Interpreter ran before valid explicit GPU selection.'
}
$run=Run-Fixture 'unlocked' '' 'GPU-AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE'
Assert ($null -eq $run.Error -and $run.Prompts -eq 0) 'Explicit UUID parameter incorrectly prompted.'
Assert ($run.Calls[1].Arguments[7] -ceq 'GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') 'Explicit UUID was not canonicalized and forwarded.'
foreach ($case in @('probe-failed','probe-unsupported','probe-noisy','store-alias')) {
    $run=Run-Fixture 'stock' $case
    Assert ($null -eq $run.Error) ('Interpreter fallback failed: '+$case+' '+$run.Error)
    $last=$run.Calls[$run.Calls.Count-1]
    Assert ($last.Kind -ceq 'plain' -and $last.Arguments[0] -ceq '-I') 'Unsupported/alias launcher was selected.'
    Assert ($last.Arguments -notcontains '-3') 'Launcher-only argument leaked to plain Python.'
    if ($case -eq 'store-alias') {Assert ($run.Calls.Count -eq 2) 'Windows Store alias was invoked.'}
}
foreach ($case in @('missing-runner','missing-python','run-failed')) {
    $run=Run-Fixture 'stock' $case
    Assert ($null -ne $run.Error) ('Launcher failure swallowed: '+$case)
    if ($case -eq 'run-failed') {Assert ($run.Error.Contains('exit code 17')) 'Native failure code was lost.'}
    else {Assert ($run.Calls.Count -eq 0) 'Interpreter ran despite missing prerequisite.'}
}
$run=Run-Fixture 'compare' 'missing-results'
Assert ($null -ne $run.Error -and $run.Error.Contains('Both a Stock and an Unlocked result')) 'Missing comparison inputs were accepted.'
Assert ($run.Calls.Count -eq 1) 'Comparison ran without both inputs.'
$run=Run-Fixture 'compare'
Assert ($null -eq $run.Error) ('Comparison launch failed: '+$run.Error)
Assert ($run.Prompts -eq 0) 'Comparison unexpectedly prompted for a GPU.'
$args=@($run.Calls[1].Arguments)
Assert ($args[3] -ceq 'compare' -and $args[4] -match 'stock-newest\.json$' -and $args[5] -match 'unlocked-newest\.json$') 'Comparison did not select newest matching results.'
Assert ($args[6] -ceq '--output-dir' -and $args[7] -match 'benchmark-results$') 'Comparison output path is wrong.'
$run=Run-Fixture 'compare' 'run-failed'
Assert ($null -ne $run.Error -and $run.Error.Contains('exit code 17')) 'Comparison process failure was swallowed.'
foreach ($pair in @(@('Benchmark-Stock.cmd','stock'),@('Benchmark-Unlocked.cmd','unlocked'),@('Compare-Benchmarks.cmd','compare'))) {
    $text=Get-Content -LiteralPath (Join-Path $PSScriptRoot ('../usb-staged/'+$pair[0])) -Raw
    Assert ($text.Contains('%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe') -and
        $text.Contains('%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe')) 'CMD wrapper does not resolve Windows PowerShell explicitly.'
    Assert ($text.Contains('-NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Benchmark.ps1" -Mode '+$pair[1])) 'CMD wrapper mode/process-only execution policy is wrong.'
    Assert ($text -match '(?m)^setlocal\r?$' -and
        $text -match 'set "GuBenchResult=%ERRORLEVEL%"\r?\npause\r?\nexit /b %GuBenchResult%') 'CMD wrapper lost scoped environment or child exit status.'
}
Write-Host ('Benchmark launcher checks passed: '+$script:Checks+'; Python, filesystem inventory and GPU execution mocked.')
