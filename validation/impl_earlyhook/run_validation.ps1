param(
  [string]$Configuration = 'Release',
  [string]$Platform = 'x64',
  [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$phase = $PSScriptRoot
$root = Split-Path (Split-Path $phase -Parent) -Parent
$build = Join-Path $phase 'build'
$runs = Join-Path $phase 'runs'
$renderdocOut = Join-Path $root "$Platform\$Configuration"
$dll = Join-Path $renderdocOut 'renderdoc.dll'
$lib = Join-Path $renderdocOut 'renderdoc.lib'
$api = Join-Path $root 'renderdoc\api'

function Invoke-Checked([string]$Label, [scriptblock]$Body) {
  Write-Host "=== $Label ==="
  & $Body
  if($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE" }
}

function Get-Capture([string]$Prefix) {
  $capture = Get-ChildItem $runs -Filter "$Prefix*.rdc" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if(-not $capture -or $capture.Length -eq 0) { throw "No capture for $Prefix" }
  return $capture.FullName
}

if(-not (Test-Path $dll) -or -not (Test-Path $lib)) {
  throw "Build RenderDoc first: expected $dll and $lib"
}

New-Item -ItemType Directory -Path $build,$runs -Force | Out-Null

Invoke-Checked 'evidence protocol tests' {
  & $Python (Join-Path $phase 'evidence_check.py') test
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs) { throw 'MSVC x64 toolchain not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$commands = @(
  "cl /nologo /W4 /EHsc /O2 validation\impl_earlyhook\capture_fixture.cpp /Fo:validation\impl_earlyhook\build\capture_fixture.obj /Fe:validation\impl_earlyhook\build\capture_fixture.exe /link d3d11.lib user32.lib",
  "cl /nologo /W3 /EHsc /O2 validation\impl_earlyhook\core_hook_fixture.cpp /Fo:validation\impl_earlyhook\build\core_hook_fixture.obj /Fe:validation\impl_earlyhook\build\core_hook_fixture.exe /link d3d11.lib dxgi.lib user32.lib",
  "cl /nologo /W3 /EHsc /O2 /D RENDERDOC_PLATFORM_WIN32 /I renderdoc\api validation\impl_earlyhook\rdc_verify.cpp /Fo:validation\impl_earlyhook\build\rdc_verify.obj /Fe:validation\impl_earlyhook\build\rdc_verify.exe /link /libpath:$Platform\$Configuration renderdoc.lib"
)

foreach($command in $commands) {
  Invoke-Checked 'fixture build' {
    & cmd.exe /d /c ('"{0}" && cd /d "{1}" && {2}' -f $vcvars,$root,$command)
  }
}

Copy-Item $dll (Join-Path $build 'renderdoc.dll') -Force
$fixture = Join-Path $build 'capture_fixture.exe'
$coreFixture = Join-Path $build 'core_hook_fixture.exe'
$verify = Join-Path $build 'rdc_verify.exe'

Invoke-Checked 'explicit capture' {
  & $fixture $dll (Join-Path $runs 'current_explicit') explicit
}
$explicit = Get-Capture 'current_explicit'
Invoke-Checked 'explicit replay verification' { & $verify $explicit 7 }

Invoke-Checked 'trigger capture' {
  & $fixture $dll (Join-Path $runs 'current_trigger') trigger
}
$trigger = Get-Capture 'current_trigger'
Invoke-Checked 'trigger replay verification' { & $verify $trigger 1 }

Invoke-Checked 'Core-hook capture' {
  & $coreFixture $dll (Join-Path $runs 'current_core')
}
$core = Get-Capture 'current_core'
Invoke-Checked 'Core-hook replay verification' { & $verify $core 7 }

Write-Host 'VALIDATION_OK protocol=9 explicit=pass trigger=pass core=pass replay=pass'
