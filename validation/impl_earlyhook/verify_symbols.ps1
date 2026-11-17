param(
  [string]$Configuration = 'Release',
  [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$phase = $PSScriptRoot
$root = Split-Path (Split-Path $phase -Parent) -Parent
$out = Join-Path $root "$Platform\$Configuration"
$dll = Join-Path $out 'renderdoc.dll'
$pdb = Join-Path $out 'renderdoc.pdb'
$symbolsTool = Join-Path $root "driver\build\$Configuration\$Platform\pdb_syms.exe"

foreach($path in @($dll,$pdb,$symbolsTool)) {
  if(-not (Test-Path $path -PathType Leaf)) { throw "Missing required file: $path" }
}

$symbols = & $symbolsTool $dll $pdb 2>&1 | Out-String
if($LASTEXITCODE -ne 0) { throw 'pdb_syms failed' }

$required = @(
  'OBJ D3D11Hook::d3d11hooks RVA ',
  'MEMBER :d3d11hooks CreateDeviceAndSwapChain OFFSET 0x8',
  'MEMBER :d3d11hooks CreateDevice OFFSET 0x10',
  'MEMBER :d3d11hooks CoreCreateDevice OFFSET 0x18',
  'OBJ DXGIHook::dxgihooks RVA ',
  'OBJ D3D12Hook::d3d12hooks RVA ',
  'SYM D3D11Hook::D3D11CreateDevice_hook RVA ',
  'SYM D3D11Hook::D3D11CreateDeviceAndSwapChain_hook RVA ',
  'SYM D3D11Hook::D3D11CreateDeviceAndSwapChainImpl_hook RVA ',
  'SYM D3D11Hook::D3D11CoreCreateDevice_hook RVA ',
  'SYM D3D12Hook::D3D12CreateDevice_hook RVA ',
  'SYM DXGIHook::CreateDXGIFactory_hook RVA ',
  'SYM DXGIHook::CreateDXGIFactory1_hook RVA ',
  'SYM DXGIHook::CreateDXGIFactory2_hook RVA ',
  'SYM DXGIHook::DXGIGetDebugInterface1_hook RVA '
)

$missing = @($required | Where-Object {
  $symbols.IndexOf($_,[StringComparison]::OrdinalIgnoreCase) -lt 0
})
if($missing.Count) { throw "Required symbols missing: $($missing -join '; ')" }

$result = [ordered]@{
  dll = $dll
  dllSha256 = (Get-FileHash $dll -Algorithm SHA256).Hash
  pdb = $pdb
  pdbSha256 = (Get-FileHash $pdb -Algorithm SHA256).Hash
  symbols = @($symbols -split "`r?`n" | Where-Object { $_ -match '^(SYM|OBJ|MEMBER) ' })
}
$result | ConvertTo-Json -Depth 4
Write-Host 'SYMBOL_GATE_OK'
