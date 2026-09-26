param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ desktop tools are required.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$vcpkg = Join-Path $vs 'VC\vcpkg'
$toolset = if (Test-Path (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v145.default.txt')) { 'v145' } else { 'v143' }
& $msbuild (Join-Path $root 'FbxViewer.vcxproj') /restore /m /p:Configuration=$Configuration /p:Platform=x64 /p:PlatformToolset=$toolset "/p:VcpkgRoot=$vcpkg\" /p:VcpkgEnableManifest=true
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
