param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $root "build\x64\$Configuration\FbxViewer.exe"
if (!(Test-Path -LiteralPath $executable)) { throw "Build the $Configuration application first." }
$process = Start-Process -FilePath $executable -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru
if (!$process.WaitForExit(30000)) {
    $process.Kill()
    throw 'Startup smoke test timed out before the first rendered frame.'
}
if ($process.ExitCode -ne 0) {
    $log = Join-Path (Split-Path $executable -Parent) 'startup.log'
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 12 }
    throw "Startup smoke test failed ($($process.ExitCode))."
}
Write-Output "PASS: $Configuration application initialized WinUI and rendered both Direct3D viewports."
