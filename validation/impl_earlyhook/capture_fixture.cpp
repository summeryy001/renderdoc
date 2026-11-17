// Self-owned D3D11 capture fixture. No remote-process access or driver loading.
// v2: adds a known triangle (green/blue/white vertices) and a frame-coded clear color,
// so a replay verifier can assert both event structure and pixel content offline.
//
// Usage: capture_fixture.exe DLL_PATH CAPTURE_PREFIX explicit|trigger
// explicit mode captures frame 7 (last frame); trigger mode captures frame 0.
// Clear color = (frame/8, 0.10, 0.10); triangle NDC verts (-0.7,-0.7) green,
// (0.7,-0.7) blue, (0.0,0.7) white.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include "../../renderdoc/api/app/renderdoc_app.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"user32.lib")

struct Vert { float pos[3]; float col[4]; };
// RT-space winding is clockwise (y-down) = D3D11 front face with default rasterizer state.
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

typedef HRESULT(WINAPI*pD3DCompile)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,LPVOID,
  LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);

static ID3D10Blob* compile(const char* src,const char* entry,const char* target,pD3DCompile fn)
{
  ID3D10Blob* code=nullptr; ID3D10Blob* err=nullptr;
  if(FAILED(fn(src,strlen(src),nullptr,nullptr,nullptr,entry,target,0,0,&code,&err))) {
    if(err) { printf("SHADER_ERROR %s\n",(const char*)err->GetBufferPointer()); err->Release(); }
    return nullptr;
  }
  if(err) err->Release();
  return code;
}

int wmain(int argc,wchar_t**argv)
{
  setvbuf(stdout,nullptr,_IONBF,0);
  // kernel-chain modes:
  //   -kernel           : no library at all (pure externally-triggered target)
  //   -kernel <dllpath> : plain-load the capture library BEFORE the gate but
  //                       never call RENDERDOC_GetAPI, so user-mode hook
  //                       registration stays dormant and the kernel trampoline
  //                       path is the ONLY hook source. LOADBASE is printed for
  //                       the arm step (wrapper = base + PDB RVA).
  if((argc==2&&wcscmp(argv[1],L"-kernel")==0)||(argc==3&&wcscmp(argv[1],L"-kernel")==0)) {
    RENDERDOC_API_1_7_0* kapi = nullptr;
    if(argc==3) {
      HMODULE hD = LoadLibraryW(argv[2]);
      if(!hD) { printf("DLL_LOAD_FAILED win32=%lu\n",GetLastError()); return 9; }
      printf("LOADBASE=%p\n",(void*)hD);
      // v22: in-process init AT GATE TIME (single thread, before any D3D work)
      // - the remote-thread GetAPI trigger raced the render thread and AV'd.
      // With RDOC_KERNEL_ARMED set, registration is suppressed so this init is
      // inert; TriggerCapture below arms the one-shot after the device exists.
      auto kget=(pRENDERDOC_GetAPI)GetProcAddress(hD,"RENDERDOC_GetAPI");
      if(kget&&kget(eRENDERDOC_API_Version_1_7_0,(void**)&kapi)==1) {
        puts("KERNEL_API_OK");
        kapi->SetCaptureFilePathTemplate("C:\\rdoc\\out\\vm_kernel");
        kapi->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation,0);
      } else { puts("KERNEL_API_FAILED"); }
    }
    puts("KERNEL_MODE");
    // gate: hold BEFORE any d3d11 involvement so the host chain can
    // freeze->inject->arm this process while it is still clean
    for(int i=0;i<1200;i++) {
      if(GetFileAttributesW(L"C:\\rdoc_gate.bin")!=INVALID_FILE_ATTRIBUTES) break;
      MSG mg; while(PeekMessageW(&mg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&mg); DispatchMessageW(&mg); }
      Sleep(100);
    }
    puts("GATE_OPEN");
    // v22.2 probe: are the d3d11/dxgi exports REALLY patched at this moment?
    {
      const char* mods[] = { "d3d11.dll", "dxgi.dll" };
      const char* fns[]  = { "D3D11CreateDeviceAndSwapChain", "CreateDXGIFactory1" };
      for(int m=0;m<2;m++) {
        HMODULE hm = GetModuleHandleA(mods[m]);
        if(!hm) { printf("PROBE %s not-loaded\n",mods[m]); continue; }
        void* fn = (void*)GetProcAddress(hm,fns[m]);
        if(!fn) { printf("PROBE %s!%s missing\n",mods[m],fns[m]); continue; }
        unsigned char* b=(unsigned char*)fn;
        printf("PROBE %s!%s @%p = %02X %02X %02X %02X %02X %02X %02X %02X\n",
               mods[m],fns[m],fn,b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
      }
    }
    WNDCLASSW wc={}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"LocalCaptureFixture";
    RegisterClassW(&wc);
    HWND wnd=CreateWindowW(wc.lpszClassName,L"Kernel-chain fixture",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
    if(!wnd) return 4;
    ShowWindow(wnd,SW_SHOW);
    DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=1; sd.BufferDesc.Width=640; sd.BufferDesc.Height=480; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=wnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap=nullptr; ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx);
    if(FAILED(hr)) { // GPU-less VM: fall back to WARP
      hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx);
      if(SUCCEEDED(hr)) puts("WARP_FALLBACK_OK");
    }
    if(FAILED(hr)) { printf("DEVICE_FAILED hr=0x%08lX\n",hr); return 5; }
    puts("DEVICE_OK");
    // v22: the device came back WRAPPED by the kernel trampoline chain - arm a
    // one-shot trigger so the next completed frame writes the .rdc (v18-style
    // auto capture, minus the timer race).
    if(kapi) { kapi->TriggerCapture(); puts("KERNEL_TRIGGER_ARMED"); }
    ID3D11Texture2D* buffer=nullptr; ID3D11RenderTargetView* rtv=nullptr;
    hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&buffer);
    if(SUCCEEDED(hr)) hr=dev->CreateRenderTargetView(buffer,nullptr,&rtv);
    if(FAILED(hr)) { printf("RTV_FAILED hr=0x%08lX\n",hr); return 6; }
    HMODULE d3dc=LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn=d3dc?(pD3DCompile)GetProcAddress(d3dc,"D3DCompile"):nullptr;
    if(!compileFn) { puts("COMPILER_FAILED"); return 6; }
    ID3D10Blob* vsb=compile(kVS,"main","vs_4_0",compileFn);
    ID3D10Blob* psb=compile(kPS,"main","ps_4_0",compileFn);
    if(!vsb||!psb) { puts("SHADER_FAILED"); return 6; }
    ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr; ID3D11InputLayout* layout=nullptr; ID3D11Buffer* vb=nullptr;
    D3D11_INPUT_ELEMENT_DESC elems[]={ {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                                       {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0} };
    hr=dev->CreateInputLayout(elems,2,vsb->GetBufferPointer(),vsb->GetBufferSize(),&layout);
    if(SUCCEEDED(hr)) hr=dev->CreateVertexShader(vsb->GetBufferPointer(),vsb->GetBufferSize(),nullptr,&vs);
    if(SUCCEEDED(hr)) hr=dev->CreatePixelShader(psb->GetBufferPointer(),psb->GetBufferSize(),nullptr,&ps);
    D3D11_BUFFER_DESC bd={}; bd.Usage=D3D11_USAGE_DEFAULT; bd.ByteWidth=sizeof(kVerts); bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init={}; init.pSysMem=kVerts;
    if(SUCCEEDED(hr)) hr=dev->CreateBuffer(&bd,&init,&vb);
    if(FAILED(hr)) { printf("PIPELINE_FAILED hr=0x%08lX\n",hr); return 6; }
    puts("TRIANGLE_PIPELINE_OK");
    UINT stride=sizeof(Vert), offset=0;
    D3D11_VIEWPORT vp={}; vp.Width=640.0f; vp.Height=480.0f; vp.MaxDepth=1.0f;
    ctx->RSSetViewports(1,&vp);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    ctx->VSSetShader(vs,nullptr,0);
    ctx->PSSetShader(ps,nullptr,0);
    for(int frame=0;frame<90;frame++) {
      MSG msg; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
      float color[4]={frame/30.0f*0.333f,0.10f,0.10f,1.0f};
      ctx->OMSetRenderTargets(1,&rtv,nullptr);
      ctx->ClearRenderTargetView(rtv,color);
      ctx->Draw(3,0);
      hr=swap->Present(0,0);
      printf("PRESENT frame=%d hr=0x%08lX\n",frame,hr);
      if(FAILED(hr)) return 7;
      Sleep(500);
    }
    puts("KERNEL_MODE_DONE");
    // v26 IAT self-scan (diagnostics, at END so the slow VAS walk never
    // delays the capture): does OUR user view contain cells equal to the
    // export address? The driver's kernel-side rescan hunts exactly these.
    {
      HMODULE hm = GetModuleHandleA("d3d11.dll");
      if(hm) {
        void* fn = (void*)GetProcAddress(hm,"D3D11CreateDeviceAndSwapChain");
        if(fn) {
          UINT64 want=(UINT64)fn; int found=0;
          SYSTEM_INFO si; GetSystemInfo(&si);
          for(UINT64 a=(UINT64)si.lpMinimumApplicationAddress; a<(UINT64)si.lpMaximumApplicationAddress && found<3; ) {
            MEMORY_BASIC_INFORMATION mbi;
            if(!VirtualQuery((LPCVOID)a,&mbi,sizeof(mbi))) break;
            if(mbi.State==MEM_COMMIT && (mbi.Protect&(PAGE_READWRITE|PAGE_READONLY|PAGE_EXECUTE_READ|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY))) {
              for(UINT64 o=0;o+8<=mbi.RegionSize && found<3;o+=8) {
                UINT64 v=0; __try { v=*(UINT64*)((UINT64)mbi.BaseAddress+o); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                if(v==want) { printf("IAT_SELFHIT cell=%p value=0x%llX\n",(void*)((UINT64)mbi.BaseAddress+o),(unsigned long long)v); found++; }
              }
            }
            a=(UINT64)mbi.BaseAddress+mbi.RegionSize;
          }
          if(!found) puts("IAT_SELFHIT none");
        }
      }
    }
    // v22.1 probes: capture-count + path + trigger-state - distinguishes
    // "device not wrapped" (0 captures) from "captured to wrong path".
    if(kapi) {
      UINT kn=kapi->GetNumCaptures();
      printf("KERNEL_CAPTURES=%u\n",kn);
      if(kn>0) {
        char kpath[4096]={}; uint32_t klen=sizeof(kpath); uint64_t kstamp=0;
        if(kapi->GetCapture(kn-1,kpath,&klen,&kstamp))
          printf("KERNEL_LAST=%s\n",kpath);
      }
    }
    rtv->Release(); buffer->Release(); vb->Release(); ps->Release(); vs->Release(); layout->Release();
    vsb->Release(); psb->Release(); ctx->Release(); swap->Release(); dev->Release(); DestroyWindow(wnd);
    return 0;
  }
  if(argc!=4) { puts("usage: capture_fixture.exe DLL_PATH CAPTURE_PREFIX explicit|trigger | -kernel"); return 1; }
  HMODULE mod=LoadLibraryW(argv[1]);
  if(!mod) { printf("LOAD_FAILED win32=%lu\n",GetLastError()); return 2; }
  auto get=(pRENDERDOC_GetAPI)GetProcAddress(mod,"RENDERDOC_GetAPI");
  RENDERDOC_API_1_7_0* api=nullptr;
  if(!get||get(eRENDERDOC_API_Version_1_7_0,(void**)&api)!=1) { puts("API_FAILED"); return 3; }
  puts("API_OK");
  char prefix[2048]={};
  WideCharToMultiByte(CP_UTF8,0,argv[2],-1,prefix,sizeof(prefix),nullptr,nullptr);
  api->SetCaptureFilePathTemplate(prefix);
  api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation,0);
  WNDCLASSW wc={}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"LocalCaptureFixture";
  RegisterClassW(&wc);
  HWND wnd=CreateWindowW(wc.lpszClassName,L"Local D3D11 capture fixture",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
  if(!wnd) return 4;
  ShowWindow(wnd,SW_SHOW);
  DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=1; sd.BufferDesc.Width=640; sd.BufferDesc.Height=480; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=wnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* swap=nullptr; ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
  HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx);
  if(FAILED(hr)) { // GPU-less VM: fall back to WARP
    hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx);
    if(SUCCEEDED(hr)) puts("WARP_FALLBACK_OK");
  }
  if(FAILED(hr)) { printf("DEVICE_FAILED hr=0x%08lX\n",hr); return 5; }
  puts("DEVICE_OK");
  ID3D11Texture2D* buffer=nullptr; ID3D11RenderTargetView* rtv=nullptr;
  hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&buffer);
  if(SUCCEEDED(hr)) hr=dev->CreateRenderTargetView(buffer,nullptr,&rtv);
  if(FAILED(hr)) { printf("RTV_FAILED hr=0x%08lX\n",hr); return 6; }
  // pipeline for the known triangle
  HMODULE d3dc=LoadLibraryW(L"d3dcompiler_47.dll");
  auto compileFn=d3dc?(pD3DCompile)GetProcAddress(d3dc,"D3DCompile"):nullptr;
  if(!compileFn) { puts("COMPILER_FAILED"); return 6; }
  ID3D10Blob* vsb=compile(kVS,"main","vs_4_0",compileFn);
  ID3D10Blob* psb=compile(kPS,"main","ps_4_0",compileFn);
  if(!vsb||!psb) { puts("SHADER_FAILED"); return 6; }
  ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr; ID3D11InputLayout* layout=nullptr; ID3D11Buffer* vb=nullptr;
  D3D11_INPUT_ELEMENT_DESC elems[]={ {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                                     {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0} };
  hr=dev->CreateInputLayout(elems,2,vsb->GetBufferPointer(),vsb->GetBufferSize(),&layout);
  if(SUCCEEDED(hr)) hr=dev->CreateVertexShader(vsb->GetBufferPointer(),vsb->GetBufferSize(),nullptr,&vs);
  if(SUCCEEDED(hr)) hr=dev->CreatePixelShader(psb->GetBufferPointer(),psb->GetBufferSize(),nullptr,&ps);
  D3D11_BUFFER_DESC bd={}; bd.Usage=D3D11_USAGE_DEFAULT; bd.ByteWidth=sizeof(kVerts); bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init={}; init.pSysMem=kVerts;
  if(SUCCEEDED(hr)) hr=dev->CreateBuffer(&bd,&init,&vb);
  if(FAILED(hr)) { printf("PIPELINE_FAILED hr=0x%08lX\n",hr); return 6; }
  puts("TRIANGLE_PIPELINE_OK");
  UINT stride=sizeof(Vert), offset=0;
  D3D11_VIEWPORT vp={}; vp.Width=640.0f; vp.Height=480.0f; vp.MaxDepth=1.0f;
  ctx->RSSetViewports(1,&vp);
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
  ctx->VSSetShader(vs,nullptr,0);
  ctx->PSSetShader(ps,nullptr,0);
  bool explicitMode=wcscmp(argv[3],L"explicit")==0;
  if(!explicitMode&&wcscmp(argv[3],L"trigger")!=0) return 1;
  UINT before=api->GetNumCaptures();
  if(!explicitMode) { api->TriggerCapture(); puts("TRIGGER_REQUEST"); }
  for(int frame=0;frame<8;frame++) {
    MSG msg; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if(explicitMode&&frame==7) { api->StartFrameCapture(dev,wnd); printf("START_REQUEST capturing=%u\n",api->IsFrameCapturing()); }
    float color[4]={frame/8.0f,0.10f,0.10f,1.0f};
    ctx->OMSetRenderTargets(1,&rtv,nullptr);
    ctx->ClearRenderTargetView(rtv,color);
    ctx->Draw(3,0);
    hr=swap->Present(0,0);
    printf("PRESENT frame=%d hr=0x%08lX capturing=%u clear_r=%.3f\n",frame,hr,api->IsFrameCapturing(),color[0]);
    if(FAILED(hr)) return 7;
  }
  if(explicitMode) printf("END_RESULT=%u\n",api->EndFrameCapture(dev,wnd));
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
  rtv->Release(); buffer->Release(); ctx->Release(); swap->Release(); dev->Release(); DestroyWindow(wnd);
  // Keep the library loaded until process exit; its background services own threads.
  return after>before&&exists?0:8;
}
