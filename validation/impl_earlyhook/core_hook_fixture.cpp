// v20 Core-hook fixture: reproduces the game's device-creation path in a
// controlled local process. Instead of calling the public D3D11CreateDevice,
// this fixture calls the DLL's exported D3D11CoreCreateDevice_hook DIRECTLY
// through the INTERNAL_Keep dllexport table - the same function the kernel
// chain redirects the game's Core calls to - using the native 10-arg ABI.
// The swapchain is then created through the (hooked) public DXGI factory,
// exactly like the engine does. No remote threads, no kernel, no driver.
//
// Usage: core_hook_fixture.exe DLL_PATH CAPTURE_PREFIX
// Exit: 0 = capture+capture-file verified; 8 = rendered but no capture;
// 9 = hook/device failure; 10 = swapchain failure.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include "../../renderdoc/api/app/renderdoc_app.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")

// native 10-arg form (see core_abi_native.h / DRIVER_RUNBOOK ABI memo):
// (creator=0, adapter, unk3=0, arg4=0, adapterCaps, pFL, layerFlags, sdk, ppDevice, pFLChosen)
typedef HRESULT(WINAPI *PFN_CORE_NATIVE)(void *, IUnknown *, void *, void *, UINT,
                                          const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                          D3D_FEATURE_LEVEL *);

struct Vert { float pos[3]; float col[4]; };
static const Vert kVerts[3] = {
  { { -0.7f, -0.7f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },   // bottom-left green
  { {  0.0f,  0.7f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } },   // top white
  { {  0.7f, -0.7f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },   // bottom-right blue
};
static const char kVS[] =
  "struct VSIn{float3 p:POSITION;float4 c:COLOR;};"
  "struct VSOut{float4 p:SV_POSITION;float4 c:COLOR;};"
  "VSOut main(VSIn i){VSOut o;o.p=float4(i.p,1);o.c=i.c;return o;}";
static const char kPS[] =
  "struct PSIn{float4 p:SV_POSITION;float4 c:COLOR;};"
  "float4 main(PSIn i):SV_Target{return i.c;}";

typedef HRESULT(WINAPI*pD3DCompile)(LPCVOID,SIZE_T,LPCSTR,const void*,void*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);

static ID3D10Blob* compile(const char* src,const char* target,pD3DCompile fn)
{
  ID3D10Blob *code=nullptr,*err=nullptr;
  if(FAILED(fn(src,strlen(src),nullptr,nullptr,nullptr,"main",target,0,0,&code,&err))) {
    if(err) { printf("SHADER_ERROR %s\n",(const char*)err->GetBufferPointer()); err->Release(); }
    return nullptr;
  }
  if(err) err->Release();
  return code;
}

int wmain(int argc,wchar_t**argv)
{
  setvbuf(stdout,nullptr,_IONBF,0);
  if(argc!=3) { puts("usage: core_hook_fixture.exe DLL_PATH CAPTURE_PREFIX"); return 1; }
  // enumerate a REAL adapter BEFORE the capture library is loaded - the game
  // passes a concrete adapter to Core (crash-dump evidence: NULL adapter makes
  // d3d11!CCreateDeviceCache::SelectAdapter deref a NULL cache object).
  if(!LoadLibraryW(L"dxgi.dll") || !LoadLibraryW(L"d3d11.dll")) return 9;
  IDXGIFactory1* rawFactory=nullptr; IDXGIAdapter1* adapter=nullptr;
  if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&rawFactory)) ||
     FAILED(rawFactory->EnumAdapters1(0,&adapter)) || !adapter) { puts("ADAPTER_ENUM_FAILED"); return 9; }
  DXGI_ADAPTER_DESC1 adesc={}; adapter->GetDesc1(&adesc);
  printf("ADAPTER_OK vid=0x%04X did=0x%04X\n",adesc.VendorId,adesc.DeviceId);
  rawFactory->Release(); rawFactory=nullptr;
  HMODULE mod=LoadLibraryW(argv[1]);
  if(!mod) { printf("LOAD_FAILED win32=%lu\n",GetLastError()); return 9; }
  auto get=(pRENDERDOC_GetAPI)GetProcAddress(mod,"RENDERDOC_GetAPI");
  RENDERDOC_API_1_7_0* api=nullptr;
  if(!get||get(eRENDERDOC_API_Version_1_7_0,(void**)&api)!=1) { puts("API_FAILED"); return 9; }
  puts("API_OK");
  char prefix[2048]={};
  WideCharToMultiByte(CP_UTF8,0,argv[2],-1,prefix,sizeof(prefix),nullptr,nullptr);
  api->SetCaptureFilePathTemplate(prefix);
  api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation,0);

  // game-faithful ordering: d3d11.dll is resident before any Core call
  if(!LoadLibraryW(L"d3d11.dll")) { printf("D3D11_LOAD_FAILED win32=%lu\n",GetLastError()); return 9; }
  puts("D3D11_RESIDENT_OK");

  // resolve the v20 Core hook through the exported keep-table:
  // [0]=D3D11CreateDevice_hook [1]=D3D11CreateDeviceAndSwapChain_hook [2]=Core hook
  void **keep=(void**)GetProcAddress(mod,"?INTERNAL_Keep@D3D11Hook@@2PAPECXA");
  if(!keep||!keep[2]) { puts("KEEP_TABLE_MISSING"); return 9; }
  PFN_CORE_NATIVE coreHook=(PFN_CORE_NATIVE)keep[2];
  printf("KEEP_TABLE_OK core_hook=%p\n",(void*)coreHook);

  // game path: device via the CORE hook with the native 10-arg ABI and a
  // concrete adapter (dump-proven: NULL adapter crashes SelectAdapter; adapter
  // in rcx yields E_INVALIDARG - the adapter lives in rdx, slot 2).
  // slot6/slot7 = feature-level ARRAY + COUNT (the +0x255 loop derefs pFL
  // walking exactly count entries; NULL array + nonzero count = NULL read).
  static const D3D_FEATURE_LEVEL kLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
  ID3D11Device* dev=nullptr; D3D_FEATURE_LEVEL fl={};
  HRESULT hr=coreHook(nullptr,adapter,nullptr,nullptr,0,kLevels,2,D3D11_SDK_VERSION,&dev,&fl);
  printf("CORE_HOOK_CALL hr=0x%08lX dev=%p fl=0x%X\n",hr,(void*)dev,fl);
  if(FAILED(hr)||!dev) return 9;
  puts("CORE_DEVICE_OK");
  ID3D11DeviceContext* ctx=nullptr;
  dev->GetImmediateContext(&ctx);
  if(!ctx) { puts("IMMEDIATE_CONTEXT_FAILED"); return 9; }
  puts("CORE_CONTEXT_OK");

  // game path: swapchain via the hooked public DXGI factory
  WNDCLASSW wc={}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"CoreHookFixture";
  RegisterClassW(&wc);
  HWND wnd=CreateWindowW(wc.lpszClassName,L"v20 Core hook fixture",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
  if(!wnd) return 9;
  ShowWindow(wnd,SW_SHOW);
  IDXGIFactory1* factory=nullptr;
  hr=CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&factory);
  if(FAILED(hr)) { printf("FACTORY_FAILED hr=0x%08lX\n",hr); return 10; }
  DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=1; sd.BufferDesc.Width=640; sd.BufferDesc.Height=480;
  sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow=wnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* swap=nullptr;
  hr=factory->CreateSwapChain(dev,&sd,&swap);
  printf("CREATE_SWAPCHAIN hr=0x%08lX swap=%p\n",hr,(void*)swap);
  if(FAILED(hr)||!swap) return 10;
  puts("SWAPCHAIN_OK");

  ID3D11Texture2D* buffer=nullptr; ID3D11RenderTargetView* rtv=nullptr;
  hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&buffer);
  if(SUCCEEDED(hr)) hr=dev->CreateRenderTargetView(buffer,nullptr,&rtv);
  if(FAILED(hr)) { printf("RTV_FAILED hr=0x%08lX\n",hr); return 10; }
  HMODULE d3dc=LoadLibraryW(L"d3dcompiler_47.dll");
  auto compileFn=d3dc?(pD3DCompile)GetProcAddress(d3dc,"D3DCompile"):nullptr;
  if(!compileFn) { puts("COMPILER_FAILED"); return 10; }
  ID3D10Blob* vsb=compile(kVS,"vs_4_0",compileFn);
  ID3D10Blob* psb=compile(kPS,"ps_4_0",compileFn);
  if(!vsb||!psb) { puts("SHADER_FAILED"); return 10; }
  ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr; ID3D11InputLayout* layout=nullptr; ID3D11Buffer* vb=nullptr;
  D3D11_INPUT_ELEMENT_DESC elems[]={ {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                                     {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0} };
  hr=dev->CreateInputLayout(elems,2,vsb->GetBufferPointer(),vsb->GetBufferSize(),&layout);
  if(SUCCEEDED(hr)) hr=dev->CreateVertexShader(vsb->GetBufferPointer(),vsb->GetBufferSize(),nullptr,&vs);
  if(SUCCEEDED(hr)) hr=dev->CreatePixelShader(psb->GetBufferPointer(),psb->GetBufferSize(),nullptr,&ps);
  D3D11_BUFFER_DESC bd={}; bd.Usage=D3D11_USAGE_DEFAULT; bd.ByteWidth=sizeof(kVerts); bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init={}; init.pSysMem=kVerts;
  if(SUCCEEDED(hr)) hr=dev->CreateBuffer(&bd,&init,&vb);
  if(FAILED(hr)) { printf("PIPELINE_FAILED hr=0x%08lX\n",hr); return 10; }
  puts("TRIANGLE_PIPELINE_OK");
  UINT stride=sizeof(Vert), offset=0;
  D3D11_VIEWPORT vp={}; vp.Width=640.0f; vp.Height=480.0f; vp.MaxDepth=1.0f;
  ctx->RSSetViewports(1,&vp);
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
  ctx->VSSetShader(vs,nullptr,0);
  ctx->PSSetShader(ps,nullptr,0);

  UINT before=api->GetNumCaptures();
  for(int frame=0;frame<8;frame++) {
    MSG msg; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if(frame==7) { api->StartFrameCapture(dev,wnd); printf("START_REQUEST capturing=%u\n",api->IsFrameCapturing()); }
    float color[4]={frame/8.0f,0.10f,0.10f,1.0f};
    ctx->OMSetRenderTargets(1,&rtv,nullptr);
    ctx->ClearRenderTargetView(rtv,color);
    ctx->Draw(3,0);
    hr=swap->Present(0,0);
    printf("PRESENT frame=%d hr=0x%08lX capturing=%u\n",frame,hr,api->IsFrameCapturing());
    if(FAILED(hr)) return 7;
  }
  printf("END_RESULT=%u\n",api->EndFrameCapture(dev,wnd));
  UINT after=api->GetNumCaptures(); printf("CAPTURES before=%u after=%u\n",before,after);
  bool exists=false;
  if(after>before) {
    char path[4096]={}; uint32_t len=sizeof(path); uint64_t stamp=0;
    if(api->GetCapture(after-1,path,&len,&stamp)) {
      wchar_t wide[4096]={}; MultiByteToWideChar(CP_UTF8,0,path,-1,wide,4096);
      WIN32_FILE_ATTRIBUTE_DATA attr={};
      exists=GetFileAttributesExW(wide,GetFileExInfoStandard,&attr)&&((attr.nFileSizeHigh!=0)||(attr.nFileSizeLow>0));
      printf("CAPTURE path=%s exists_nonempty=%d\n",path,exists);
    }
  }
  vb->Release(); ps->Release(); vs->Release(); layout->Release(); vsb->Release(); psb->Release();
  rtv->Release(); buffer->Release(); ctx->Release(); swap->Release(); factory->Release(); dev->Release(); DestroyWindow(wnd);
  return after>before&&exists?0:8;
}
