param(
  [Parameter(Mandatory=$true)] [string]$MapperPath,
  [string]$TargetProcessName = 'Client-Win64-Shipping*',
  [UInt64]$TargetImplRva = 0x16940,
  [string]$Configuration = 'Release',
  [string]$Platform = 'x64'
)

# Wrap D3D11CreateDeviceAndSwapChainImpl after the system has finished
# constructing its private interfaces, but before the public device is returned.
$ErrorActionPreference = 'Continue'
$root   = Split-Path $PSScriptRoot -Parent
$out    = Join-Path $root "$Platform\$Configuration"
$tools  = Join-Path $root "driver\build\$Configuration\$Platform"
$cmd    = Join-Path $out 'renderdoccmd.exe'
$ctl    = Join-Path $tools 'rdocctl.exe'
$probe  = Join-Path $tools 'rdcprobe.exe'
$syms   = Join-Path $tools 'pdb_syms.exe'
$dll    = Join-Path $out 'renderdoc.dll'
$pdb    = Join-Path $out 'renderdoc.pdb'
$driver = Join-Path $tools 'rdoc_earlyhook.sys'
$mapper = (Resolve-Path $MapperPath -ErrorAction Stop).Path
$desk   = [Environment]::GetFolderPath('Desktop')
$round  = Join-Path $root ("validation\impl_earlyhook\live_runs\{0:yyyyMMdd_HHmmss}_impl" -f (Get-Date))
New-Item -ItemType Directory -Path $round -Force | Out-Null
$timeline = Join-Path $round 'timeline.csv'
'monotonic_ms,wall_time,event,detail' | Out-File $timeline -Encoding ascii
$clock = [Diagnostics.Stopwatch]::StartNew()
function Mark([string]$event, [string]$detail = '') {
  $safe = $detail.Replace('"','""')
  ('{0},"{1:O}","{2}","{3}"' -f $clock.ElapsedMilliseconds,(Get-Date),$event,$safe) |
    Add-Content $timeline -Encoding ascii
  Write-Host ("RDOC_DIAG {0} t={1}ms {2}" -f $event,$clock.ElapsedMilliseconds,$detail)
}
$identity = @($dll,$pdb,$driver,$ctl,$probe,$syms,$mapper) | ForEach-Object {
  $item = Get-Item $_ -ErrorAction Stop
  [pscustomobject]@{ path=$item.FullName; length=$item.Length; sha256=(Get-FileHash $item.FullName -Algorithm SHA256).Hash }
}
$identity | ConvertTo-Json | Out-File (Join-Path $round 'build_identity.json') -Encoding utf8
New-Item -ItemType File -Path C:\rdoc_kernel_single_layer.flag -Force | Out-Null
Mark 'PREFLIGHT_OK' "round=$round mode=KernelSingleLayer"

function Get-SymbolRva([string]$name) {
  $match = [regex]::Match($symbols, "(?im)^SYM $([regex]::Escape($name)) RVA 0x([0-9a-f]+)\r?$")
  if(-not $match.Success) { throw "Missing symbol: $name" }
  return [Convert]::ToUInt64($match.Groups[1].Value,16)
}
function Get-ObjectRva([string]$name) {
  $match = [regex]::Match($symbols, "(?im)^OBJ $([regex]::Escape($name)) RVA 0x([0-9a-f]+)\r?$")
  if(-not $match.Success) { throw "Missing object: $name" }
  return [Convert]::ToUInt64($match.Groups[1].Value,16)
}
function Get-MemberOffset([string]$owner, [string]$name) {
  $match = [regex]::Match($symbols, "(?im)^MEMBER [^\r\n]*$([regex]::Escape($owner)) $([regex]::Escape($name)) OFFSET 0x([0-9a-f]+)\r?$")
  if(-not $match.Success) { throw "Missing member: $owner.$name" }
  return [Convert]::ToUInt64($match.Groups[1].Value,16)
}

$symbols = & $syms $dll $pdb 2>&1 | Where-Object { $_ -match '^(SYM|OBJ|MEMBER) ' } | Out-String
$symbols | Out-File (Join-Path $round 'symbols.txt') -Encoding ascii

try {
  $d3d11Object = Get-ObjectRva 'D3D11Hook::d3d11hooks'
  $dxgiObject = Get-ObjectRva 'DXGIHook::dxgihooks'
  $d3d12Object = Get-ObjectRva 'D3D12Hook::d3d12hooks'
  $RVA = @{
    w_CreateDXGIFactory  = Get-SymbolRva 'DXGIHook::CreateDXGIFactory_hook'; o_CreateDXGIFactory  = $dxgiObject + (Get-MemberOffset 'dxgihooks' 'CreateDXGIFactory')
    w_CreateDXGIFactory1 = Get-SymbolRva 'DXGIHook::CreateDXGIFactory1_hook'; o_CreateDXGIFactory1 = $dxgiObject + (Get-MemberOffset 'dxgihooks' 'CreateDXGIFactory1')
    w_CreateDXGIFactory2 = Get-SymbolRva 'DXGIHook::CreateDXGIFactory2_hook'; o_CreateDXGIFactory2 = $dxgiObject + (Get-MemberOffset 'dxgihooks' 'CreateDXGIFactory2')
    w_GetDbgIface1       = Get-SymbolRva 'DXGIHook::DXGIGetDebugInterface1_hook'; o_GetDbgIface1 = $dxgiObject + (Get-MemberOffset 'dxgihooks' 'GetDebugInterface1')
    w_D3D11CreateDevice  = Get-SymbolRva 'D3D11Hook::D3D11CreateDevice_hook'; o_D3D11CreateDevice = $d3d11Object + (Get-MemberOffset 'd3d11hooks' 'CreateDevice')
    w_D3D11CreateDevSC   = Get-SymbolRva 'D3D11Hook::D3D11CreateDeviceAndSwapChain_hook'; o_D3D11CreateDevSC = $d3d11Object + (Get-MemberOffset 'd3d11hooks' 'CreateDeviceAndSwapChain')
    w_D3D11CreateImpl    = Get-SymbolRva 'D3D11Hook::D3D11CreateDeviceAndSwapChainImpl_hook'; o_D3D11CreateImpl = $d3d11Object + (Get-MemberOffset 'd3d11hooks' 'CoreCreateDevice')
    w_D3D12CreateDevice  = Get-SymbolRva 'D3D12Hook::D3D12CreateDevice_hook'; o_D3D12CreateDevice = $d3d12Object + (Get-MemberOffset 'd3d12hooks' 'CreateDevice')
  }
} catch {
  Mark 'RVA_IDENTITY_FAILED' $_.Exception.Message
  Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue
  exit 10
}
Mark 'RVA_IDENTITY_OK' 'symbols, objects, and member offsets derived from DLL/PDB'


Add-Type -Namespace Native -Name Nt @"
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
[DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
"@

Write-Host "=== [1] driver (skip map if heartbeat fresh - no double mapping) ==="
if ((Test-Path C:\rdoc_status.bin) -and ((Get-Item C:\rdoc_status.bin).LastWriteTime -gt (Get-Date).AddSeconds(-30))) {
  Write-Host 'driver already alive (fresh heartbeat) - reusing'
} else {
  Remove-Item C:\rdoc_status.bin -Force -ErrorAction SilentlyContinue
  & $mapper $driver 2>&1 | Select-Object -Last 2 | Write-Host
  Start-Sleep -Seconds 6
}
if (-not (Test-Path C:\rdoc_status.bin) -or ((Get-Item C:\rdoc_status.bin).LastWriteTime -lt (Get-Date).AddSeconds(-30))) { Mark 'MAP_FAILED'; Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue; exit 1 }
Mark 'DRIVER_ALIVE'

Write-Host '=== [2] OPERATOR: launch game DX11 NOW ==='
$g = $null; $deadline = (Get-Date).AddMinutes(5)
while ((Get-Date) -lt $deadline) {
  $g = Get-Process | Where-Object { $_.Name -like $TargetProcessName -and $_.StartTime -gt (Get-Date).AddSeconds(-10) } | Select-Object -First 1
  if ($g) { break }
  Start-Sleep -Milliseconds 150
}
if (-not $g) { Mark 'GAME_NOT_FOUND'; & $ctl disarmfile | Out-Host; Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue; exit 1 }
$pid2 = $g.Id
Mark 'PROCESS_FOUND' "pid=$pid2 start=$($g.StartTime.ToString('O'))"
$h = [Native.Nt]::OpenProcess(0x1F0FFF, $false, $pid2)
if ($h -eq [IntPtr]::Zero) { Mark 'OPEN_PROCESS_FAILED'; Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue; exit 2 }

Write-Host '=== [3] inject (no freeze, 60s timeout, verbose trace) ==='
Mark 'INJECT_BEGIN'
$env:RDOC_STEALTH_VERBOSE = '1'
$injectProc = Start-Process -FilePath $cmd -ArgumentList @("inject","--PID=$pid2","--capture-file=$desk\capture_kernel") -PassThru -WindowStyle Hidden -RedirectStandardOutput "$env:TEMP\inject_out7.txt" -RedirectStandardError "$env:TEMP\inject_err7.txt" -ErrorAction SilentlyContinue
if ($injectProc -and -not $injectProc.WaitForExit(60000)) {
  Mark 'INJECT_TIMEOUT'
  Write-Host 'INJECT TIMEOUT'; Stop-Process -Id $injectProc.Id -Force -ErrorAction SilentlyContinue
  [void][Native.Nt]::NtResumeProcess($h); [void][Native.Nt]::CloseHandle($h); & $ctl disarmfile | Out-Null; Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue; exit 3
}
Remove-Item Env:RDOC_STEALTH_VERBOSE -ErrorAction SilentlyContinue
$injectOut = Get-Content "$env:TEMP\inject_out7.txt","$env:TEMP\inject_err7.txt" -ErrorAction SilentlyContinue | Out-String
Write-Host ($injectOut.Trim() -split "`r?`n" | Select-Object -Last 6) -join ' | '
$base = $null
foreach($pat in @('module base 0x([0-9a-fA-F]+)','module=0x([0-9a-fA-F]+)','hidden module base 0x([0-9a-fA-F]+)')){ if($injectOut -match $pat){ $base=[Convert]::ToUInt64($Matches[1],16); break } }
# v25.1: legacy-path fallback ("Launched as ID") - poll the game's modules for
# the injected copy (Temp path, ~25MB) for up to 20s; the async load reports
# its module once mapped.
if (-not $base -and $injectOut -match 'Launched as ID') {
  Write-Host 'legacy path - polling modules for the copy...'
  foreach($w in 1..20) {
    Start-Sleep -Seconds 1
    $gp = Get-Process -Id $pid2 -ErrorAction SilentlyContinue
    if (-not $gp) { break }
    foreach($m in $gp.Modules){ if($m.FileName -match '\\Temp\\' -and $m.ModuleMemorySize -gt 20MB){ $base = $m.BaseAddress.ToInt64(); break } }
    if ($base) { break }
  }
}
if (-not $base) { Mark 'INJECT_FAILED'; [void][Native.Nt]::NtResumeProcess($h); [void][Native.Nt]::CloseHandle($h); & $ctl disarmfile | Out-Host; Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue; exit 3 }
Mark 'DLL_READY' ("base=0x{0:X}" -f $base)
Remove-Item C:\rdoc_kernel_single_layer.flag -Force -ErrorAction SilentlyContinue
[void][Native.Nt]::CloseHandle($h)

Write-Host '=== [4] ARM ==='
$armArgs = @('armfile', "--pid=$pid2",
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=dxgi.dll func=CreateDXGIFactory'  -f (($base+$RVA.w_CreateDXGIFactory), ($base+$RVA.o_CreateDXGIFactory))),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=dxgi.dll func=CreateDXGIFactory1' -f (($base+$RVA.w_CreateDXGIFactory1),($base+$RVA.o_CreateDXGIFactory1))),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=dxgi.dll func=CreateDXGIFactory2' -f (($base+$RVA.w_CreateDXGIFactory2),($base+$RVA.o_CreateDXGIFactory2))),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=dxgi.dll func=DXGIGetDebugInterface1' -f (($base+$RVA.w_GetDbgIface1),($base+$RVA.o_GetDbgIface1))),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=d3d11.dll func=D3D11CreateDevice' -f (($base+$RVA.w_D3D11CreateDevice),($base+$RVA.o_D3D11CreateDevice))),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=d3d11.dll func=D3D11CreateDeviceAndSwapChain' -f (($base+$RVA.w_D3D11CreateDevSC),($base+$RVA.o_D3D11CreateDevSC))),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=d3d11.dll func=@rva:{2:X}' -f (($base+$RVA.w_D3D11CreateImpl),($base+$RVA.o_D3D11CreateImpl),$TargetImplRva)),
  ('--entry=wrapper=0x{0:X} orig=0x{1:X} dll=d3d12.dll func=D3D12CreateDevice' -f (($base+$RVA.w_D3D12CreateDevice),($base+$RVA.o_D3D12CreateDevice))))
$armOut = & $ctl armfile @armArgs 2>&1 | Out-String
Write-Host "ARM: $($armOut.Trim())"
Mark 'ARM_REQUESTED' $armOut.Trim()
$armSeq = if ($armOut -match 'seq\s+(\d+)') { [int]$Matches[1] } else { -1 }
$driverState = ''
$armedReady = $false
$armDeadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $armDeadline) {
  Start-Sleep -Milliseconds 250
  $driverState = & $ctl readstatus 2>&1 | Out-String
  $ack = if ($driverState -match 'ack=(\d+)') { [int]$Matches[1] } else { -1 }
  $detours = ([regex]::Matches($driverState, '(?m)^\s+\[\d+\].*\bDETOUR\b')).Count
  if ($armSeq -gt 0 -and $ack -eq $armSeq -and $detours -gt 0) { $armedReady = $true; break }
}
$driverState | Out-File (Join-Path $round 'driver_state_after_arm.txt') -Encoding utf8
if (-not $armedReady) {
  Mark 'ARM_NOT_READY' "seq=$armSeq"
  & $ctl disarmfile | Out-Null
  exit 4
}
Mark 'HOOK_INSTALLED' "ack=$armSeq detours=$detours"

Write-Host '=== [5] poll target-control readiness (not a wrap signal) ==='
$targetControlReady = $false; $t0 = Get-Date
while (((Get-Date)-$t0).TotalSeconds -lt 60) {
  Start-Sleep -Milliseconds 500
  if (-not (Get-Process -Id $pid2 -ErrorAction SilentlyContinue)) { Mark 'GAME_DIED'; break }
  $tc = Get-NetTCPConnection -LocalPort 38920 -State Listen -ErrorAction SilentlyContinue | Where-Object {$_.OwningProcess -eq $pid2}
  if ($tc) { $targetControlReady = $true; Mark 'TARGET_CONTROL_READY' 'port=38920'; break }
}
if (-not $targetControlReady) { Mark 'TARGET_CONTROL_NOT_READY'; & $ctl disarmfile | Out-Host; exit 5 }

Write-Host '=== [6] wait for API registration, then trigger ==='
$pr = & $probe 15000 5000 127.0.0.1 38920 require-api 2>&1 | Out-String
$probeExit = $LASTEXITCODE
$pr | Out-File (Join-Path $round 'target_control.txt') -Encoding utf8
Write-Host $pr.Trim()
$apiRegistered = $probeExit -ne 3 -and $pr -match 'RDOC_DIAG API_REGISTERED'
if (-not $apiRegistered) {
  Mark 'API_NOT_REGISTERED' 'external trigger suppressed; waiting for in-process auto capture'
  $finalState = & $ctl readstatus 2>&1 | Out-String
  $finalState | Out-File (Join-Path $round 'driver_state_api_none.txt') -Encoding utf8
  & $ctl disarmfile | Out-Null
  Copy-Item C:\rdoc_status.bin (Join-Path $round 'rdoc_status.bin') -Force -ErrorAction SilentlyContinue
} else {
  Mark 'API_REGISTERED' (($pr -split "`r?`n" | Where-Object {$_ -match 'RDOC_DIAG API_REGISTERED'} | Select-Object -Last 1))
  Mark 'CAPTURE_TRIGGER_SENT'
}

Write-Host '=== [7] wait for .rdc (external or 45s in-process trigger) ==='
$rdc = $null; $t0 = Get-Date
while (((Get-Date)-$t0).TotalSeconds -lt 120) {
  Start-Sleep -Seconds 5
  $rdc = Get-ChildItem "$desk\*.rdc" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0.AddMinutes(-3) } | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($rdc -and $rdc.Length -gt 0) { break }
}

Write-Host '=== [8] disarm + collect ==='
& $ctl disarmfile | Out-Null
Copy-Item C:\rdoc_status.bin (Join-Path $round 'rdoc_status.bin') -Force -ErrorAction SilentlyContinue
(& $ctl readstatus 2>&1 | Out-String) | Out-File (Join-Path $round 'driver_state_final.txt') -Encoding utf8
if ($rdc) {
  $allCaptures = Get-ChildItem "$desk\*.rdc" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0.AddMinutes(-3) } | Sort-Object LastWriteTime
  $allCaptures | Select-Object FullName,Length,LastWriteTime | Export-Csv (Join-Path $round 'captures.csv') -NoTypeInformation -Encoding UTF8
  Copy-Item $rdc.FullName (Join-Path $round $rdc.Name) -Force
  Mark 'CAPTURE_WRITTEN' "path=$($rdc.FullName) bytes=$($rdc.Length)"
  Write-Host "CAPTURE: $($rdc.FullName) ($([math]::Round($rdc.Length/1MB,2)) MB)"
  $thumb = Join-Path $round 'cap_thumb.png'
  & $cmd thumb "--out=$thumb" -f png $rdc.FullName 2>&1 | Select-Object -Last 1 | Write-Host
  'ok' | Out-File (Join-Path $round 'result.txt') -Encoding ascii
  exit 0
}
Mark 'CAPTURE_NOT_WRITTEN'
'capture-not-written' | Out-File (Join-Path $round 'result.txt') -Encoding ascii
exit 7
