param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$Hover)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $root "build\x64\$Configuration\FbxViewer.exe"
if (!(Test-Path -LiteralPath $executable)) { throw "Build the $Configuration application first." }
$argument = if ($Hover) { '--hover-test' } else { '--smoke-test' }
$process = Start-Process -FilePath $executable -ArgumentList $argument -WindowStyle Hidden -PassThru
if (!$process.WaitForExit(30000)) {
    $process.Kill()
    throw "Application test timed out ($argument)."
}
if ($process.ExitCode -ne 0) {
    $log = Join-Path (Split-Path $executable -Parent) 'startup.log'
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 12 }
    throw "Application test failed ($argument, exit $($process.ExitCode))."
}
Write-Output "PASS: $Configuration application test ($argument)."
