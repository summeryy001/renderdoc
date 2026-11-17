param(
  [string]$Configuration = 'Release',
  [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $PSScriptRoot "build\$Configuration\$Platform"
New-Item -ItemType Directory -Path $out -Force | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs) { throw 'MSVC x64 toolchain not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$commands = @(
  "cl /nologo /W4 /O2 /D_CRT_SECURE_NO_WARNINGS driver\rdocctl\rdocctl.c /Fo:driver\build\$Configuration\$Platform\rdocctl.obj /Fe:driver\build\$Configuration\$Platform\rdocctl.exe",
  "cl /nologo /W4 /O2 driver\rdcprobe.c /Fo:driver\build\$Configuration\$Platform\rdcprobe.obj /Fe:driver\build\$Configuration\$Platform\rdcprobe.exe ws2_32.lib",
  "cl /nologo /W4 /O2 /D_CRT_SECURE_NO_WARNINGS driver\pdb_syms\pdb_syms.c /Fo:driver\build\$Configuration\$Platform\pdb_syms.obj /Fe:driver\build\$Configuration\$Platform\pdb_syms.exe dbghelp.lib"
)

foreach($command in $commands) {
  & cmd.exe /d /c ('"{0}" && cd /d "{1}" && {2}' -f $vcvars,$root,$command)
  if($LASTEXITCODE -ne 0) { throw "Tool build failed: $command" }
}

Write-Host "TOOLS_BUILD_OK output=$out"
