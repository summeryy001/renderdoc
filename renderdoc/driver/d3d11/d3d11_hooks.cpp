/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "d3d11_hooks.h"
#include "driver/dxgi/dxgi_wrapped.h"
#include "hooks/hooks.h"
#include "d3d11_device.h"
#include <intrin.h>
#include <thread>

typedef HRESULT(WINAPI *PFN_D3D11_CORE_CREATE_DEVICE)(
    __in_opt void *creator_unknown, __in_opt IUnknown *pAdapter, __in_opt void *unk3_zero,
    __in_opt void *arg4_unknown, UINT createFlags,
    __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, __out_opt ID3D11Device **ppDevice,
    __out_opt D3D_FEATURE_LEVEL *pFeatureLevel);

ID3DDevice *GetD3D11DeviceIfAlloc(IUnknown *dev)
{
  if(WrappedID3D11Device::IsAlloc(dev))
    return (WrappedID3D11Device *)dev;

  return NULL;
}

class D3D11Hook : LibraryHook
{
public:
  void RegisterHooks()
  {
    RDCLOG("Registering D3D11 hooks");

    WrappedIDXGISwapChain4::RegisterD3DDeviceCallback(GetD3D11DeviceIfAlloc);

    // also require d3dcompiler_??.dll
    if(GetD3DCompiler() == NULL)
    {
      RDCERR("Failed to load d3dcompiler_??.dll - not inserting D3D11 hooks.");
      return;
    }

    LibraryHooks::RegisterLibraryHook("d3d11.dll", NULL);

    const bool kernelSingleLayer =
        GetFileAttributesA("C:\\rdoc_kernel_single_layer.flag") != INVALID_FILE_ATTRIBUTES;

    RDCLOG("RDOC_DIAG HOOK_MODE=%s", kernelSingleLayer ? "KernelSingleLayer" : "Normal");

    if(kernelSingleLayer)
    {
      RDCLOG("RDOC_DIAG USER_HOOKS_SUPPRESSED api=D3D11 reason=KernelSingleLayer");
      m_RecurseSlot = Threading::AllocateTLSSlot();
      Threading::SetTLSValue(m_RecurseSlot, NULL);
      return;
    }

    CreateDeviceAndSwapChain.Register("d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                                      D3D11CreateDeviceAndSwapChain_hook);
    CreateDevice.Register("d3d11.dll", "D3D11CreateDevice", D3D11CreateDevice_hook);

    m_RecurseSlot = Threading::AllocateTLSSlot();
    Threading::SetTLSValue(m_RecurseSlot, NULL);
    RDCLOG("RDOC_DIAG USER_HOOKS_REGISTERED api=D3D11");
  }

private:
  static D3D11Hook d3d11hooks;

public:
  __declspec(dllexport) static volatile void *INTERNAL_Keep[4];

  HookedFunction<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN> CreateDeviceAndSwapChain;
  HookedFunction<PFN_D3D11_CREATE_DEVICE> CreateDevice;
  HookedFunction<PFN_D3D11_CORE_CREATE_DEVICE> CoreCreateDevice;

  static HRESULT WINAPI D3D11CreateDeviceAndSwapChainImpl_hook(
      __in_opt IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
      __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
      __out_opt IDXGISwapChain **ppSwapChain, __out_opt ID3D11Device **ppDevice,
      __out_opt D3D_FEATURE_LEVEL *pFeatureLevel, __out_opt ID3D11DeviceContext **ppImmediateContext);

  static HRESULT WINAPI D3D11CoreCreateDevice_hook(
      __in_opt void *creator_unknown, __in_opt IUnknown *pAdapter, __in_opt void *unk3_zero,
      __in_opt void *arg4_unknown, UINT createFlags,
      __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, __out_opt ID3D11Device **ppDevice,
      __out_opt D3D_FEATURE_LEVEL *pFeatureLevel);

  // re-entrancy detection (can happen in rare cases with e.g. fraps)
  uint64_t m_RecurseSlot = 0;

  void EndRecurse() { Threading::SetTLSValue(m_RecurseSlot, NULL); }
  bool CheckRecurse()
  {
    if(Threading::GetTLSValue(m_RecurseSlot) == NULL)
    {
      Threading::SetTLSValue(m_RecurseSlot, (void *)1);
      return false;
    }

    return true;
  }

  friend HRESULT CreateD3D11_Internal(RealD3D11CreateFunction real, __in_opt IDXGIAdapter *pAdapter,
                                      D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                                      __in_ecount_opt(FeatureLevels)
                                          CONST D3D_FEATURE_LEVEL *pFeatureLevels,
                                      UINT FeatureLevels, UINT SDKVersion,
                                      __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                                      __out_opt IDXGISwapChain **ppSwapChain,
                                      __out_opt ID3D11Device **ppDevice,
                                      __out_opt D3D_FEATURE_LEVEL *pFeatureLevel,
                                      __out_opt ID3D11DeviceContext **ppImmediateContext);

  HRESULT Create_Internal(RealD3D11CreateFunction real, __in_opt IDXGIAdapter *pAdapter,
                          D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                          __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels,
                          UINT FeatureLevels, UINT SDKVersion,
                          __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                          __out_opt IDXGISwapChain **ppSwapChain, __out_opt ID3D11Device **ppDevice,
                          __out_opt D3D_FEATURE_LEVEL *pFeatureLevel,
                          __out_opt ID3D11DeviceContext **ppImmediateContext)
  {
    // if we're already inside a wrapped create, then DON'T do anything special. Just call onwards
    if(CheckRecurse())
    {
      return real(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                  pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    }

    RDCDEBUG("Call to Create_Internal Flags %x", Flags);

    // we should no longer go through here in the replay application
    RDCASSERT(!RenderDoc::Inst().IsReplayApp());

    if(RenderDoc::Inst().GetCaptureOptions().apiValidation &&
       GetModuleHandleA("d3d11_3sdklayers.dll") != NULL)
      Flags |= D3D11_CREATE_DEVICE_DEBUG;
    else
      Flags &= ~D3D11_CREATE_DEVICE_DEBUG;

    DXGI_SWAP_CHAIN_DESC swapDesc;
    DXGI_SWAP_CHAIN_DESC *pUsedSwapDesc = NULL;

    if(pSwapChainDesc)
    {
      swapDesc = *pSwapChainDesc;
      pUsedSwapDesc = &swapDesc;
    }

    if(pUsedSwapDesc && !RenderDoc::Inst().GetCaptureOptions().allowFullscreen)
    {
      pUsedSwapDesc->Windowed = TRUE;
    }

    RDCLOG("RDOC_DIAG CREATE_INTERNAL_ENTER flags=0x%x feature_levels=%u", Flags, FeatureLevels);
    RDCDEBUG("Calling real createdevice...");

    // Hack for D3DGear which crashes if ppDevice is NULL
    ID3D11Device *dummydev = NULL;
    bool dummyUsed = false;
    if(ppDevice == NULL)
    {
      ppDevice = &dummydev;
      dummyUsed = true;
    }

    HRESULT ret = real(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                       SDKVersion, pUsedSwapDesc, ppSwapChain, ppDevice, pFeatureLevel, NULL);

    SAFE_RELEASE(dummydev);
    if(dummyUsed)
      ppDevice = NULL;

    RDCLOG("RDOC_DIAG REAL_CREATE_RETURN hr=0x%08x device=%p swapchain=%p", ret,
           ppDevice ? *ppDevice : NULL, ppSwapChain ? *ppSwapChain : NULL);
    RDCDEBUG("Called real createdevice...");

    bool suppress = false;

    suppress = (Flags & D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY) != 0;

    if(suppress)
    {
      RDCLOG("Application requested not to be hooked.");
    }
    else if(SUCCEEDED(ret) && ppDevice)
    {
      RDCDEBUG("succeeded and hooking.");

      if(!WrappedID3D11Device::IsAlloc(*ppDevice))
      {
        D3D11InitParams params;
        params.DriverType = DriverType;
        params.Flags = Flags;
        params.SDKVersion = SDKVersion;
        params.NumFeatureLevels = FeatureLevels;
        if(FeatureLevels > 0)
          memcpy(params.FeatureLevels, pFeatureLevels, sizeof(D3D_FEATURE_LEVEL) * FeatureLevels);

        WrappedID3D11Device *wrap = new WrappedID3D11Device(*ppDevice, params);

        RDCLOG("RDOC_DIAG DEVICE_WRAPPED api=D3D11 real=%p wrapped=%p", *ppDevice, wrap);
        RDCDEBUG("created wrapped device.");

        *ppDevice = wrap;

        {
          static bool autoCaptureStarted = false;
          if(!autoCaptureStarted)
          {
            autoCaptureStarted = true;
            RDCLOG("RDOC_DIAG AUTO_CAPTURE_ARMED delay_ms=45000");
            std::thread t([]() {
              Threading::Sleep(45000);
              RDCLOG("RDOC_DIAG AUTO_CAPTURE_TRIGGER frames=1");
              RenderDoc::Inst().TriggerCapture(1);
            });
            t.detach();
          }
        }

        if(ppImmediateContext)
          wrap->GetImmediateContext(ppImmediateContext);

        if(ppSwapChain && *ppSwapChain)
          *ppSwapChain = new WrappedIDXGISwapChain4(
              *ppSwapChain, pSwapChainDesc ? pSwapChainDesc->OutputWindow : NULL, wrap);
      }
    }
    else if(SUCCEEDED(ret))
    {
      RDCLOG("Created wrapped D3D11 device.");
    }
    else
    {
      RDCDEBUG("failed. HRESULT: %s", ToStr(ret).c_str());
    }

    EndRecurse();

    return ret;
  }

  static HRESULT WINAPI D3D11CreateDevice_hook(
      __in_opt IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
      __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, __out_opt ID3D11Device **ppDevice,
      __out_opt D3D_FEATURE_LEVEL *pFeatureLevel, __out_opt ID3D11DeviceContext **ppImmediateContext)
  {
    RDCLOG("RDOC_DIAG ENTRY_PUBLIC_DEVICE caller=%p", _ReturnAddress());
    // just forward the call with NULL swapchain parameters
    return D3D11CreateDeviceAndSwapChain_hook(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                              FeatureLevels, SDKVersion, NULL, NULL, ppDevice,
                                              pFeatureLevel, ppImmediateContext);
  }

  static HRESULT WINAPI D3D11CreateDeviceAndSwapChain_hook(
      __in_opt IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
      __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
      __out_opt IDXGISwapChain **ppSwapChain, __out_opt ID3D11Device **ppDevice,
      __out_opt D3D_FEATURE_LEVEL *pFeatureLevel, __out_opt ID3D11DeviceContext **ppImmediateContext)
  {
    RDCLOG("RDOC_DIAG ENTRY_PUBLIC_SWAPCHAIN caller=%p swap_desc=%p", _ReturnAddress(),
           pSwapChainDesc);
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN createFunc = d3d11hooks.CreateDeviceAndSwapChain();

    if(createFunc == NULL)
    {
      RDCWARN("Call to D3D11CreateDeviceAndSwapChain_hook without onward function pointer");
      createFunc = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(
          GetModuleHandleA("d3d11.dll"), "D3D11CreateDeviceAndSwapChain");
    }

    // shouldn't ever get here, we should either have it from procaddress or the hook function, but
    // let's be safe.
    if(createFunc == NULL)
    {
      RDCERR("Something went seriously wrong with the hooks!");
      return E_UNEXPECTED;
    }

    return d3d11hooks.Create_Internal(createFunc, pAdapter, DriverType, Software, Flags,
                                      pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
                                      ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
  }
};

D3D11Hook D3D11Hook::d3d11hooks;

static PFN_D3D11_CORE_CREATE_DEVICE g_coreReal = NULL;

static HRESULT WINAPI D3D11CoreRealBridge(
    __in_opt IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    __out_opt IDXGISwapChain **ppSwapChain, __out_opt ID3D11Device **ppDevice,
    __out_opt D3D_FEATURE_LEVEL *pFeatureLevel, __out_opt ID3D11DeviceContext **ppImmediateContext)
{
  (void)DriverType;
  (void)Software;
  (void)Flags;
  (void)pSwapChainDesc;
  (void)ppSwapChain;
  (void)ppImmediateContext;

  PFN_D3D11_CORE_CREATE_DEVICE core = g_coreReal;
  if(!core)
  {
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if(d3d11)
      core = (PFN_D3D11_CORE_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CoreCreateDevice");
  }
  if(!core)
    return E_NOINTERFACE;

  IDXGIAdapter *adapter = pAdapter;
  if(!adapter)
  {
    typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void **);
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if(!dxgi)
      dxgi = LoadLibraryW(L"dxgi.dll");
    if(dxgi)
    {
      PFN_CREATE_DXGI_FACTORY1 fac1 =
          (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(dxgi, "CreateDXGIFactory1");
      IDXGIFactory1 *factory = NULL;
      if(fac1 && SUCCEEDED(fac1(__uuidof(IDXGIFactory1), (void **)&factory)) && factory)
      {
        IDXGIAdapter1 *def = NULL;
        if(SUCCEEDED(factory->EnumAdapters1(0, &def)) && def)
          adapter = def;
        factory->Release();
      }
    }
  }

  return core(NULL, adapter, NULL, NULL, 0, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice,
              pFeatureLevel);
}

HRESULT D3D11Hook::D3D11CreateDeviceAndSwapChainImpl_hook(
    __in_opt IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    __out_opt IDXGISwapChain **ppSwapChain, __out_opt ID3D11Device **ppDevice,
    __out_opt D3D_FEATURE_LEVEL *pFeatureLevel, __out_opt ID3D11DeviceContext **ppImmediateContext)
{
  PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN createFunc =
      (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)d3d11hooks.CoreCreateDevice();
  if(createFunc == NULL)
  {
    RDCERR("D3D11CreateDeviceAndSwapChainImpl onward pointer unavailable");
    return E_UNEXPECTED;
  }

  RDCLOG("RDOC_DIAG ENTRY_IMPL caller=%p flags=0x%x swap_desc=%p", _ReturnAddress(), Flags,
         pSwapChainDesc);
  return d3d11hooks.Create_Internal(createFunc, pAdapter, DriverType, Software, Flags,
                                    pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
                                    ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
}

HRESULT D3D11Hook::D3D11CoreCreateDevice_hook(
    __in_opt void *creator_unknown, __in_opt IUnknown *pAdapter, __in_opt void *unk3_zero,
    __in_opt void *arg4_unknown, UINT createFlags,
    __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, __out_opt ID3D11Device **ppDevice, __out_opt D3D_FEATURE_LEVEL *pFeatureLevel)
{
  RDCLOG("RDOC_DIAG ENTRY_CORE caller=%p creator=%p adapter=%p flags=0x%x", _ReturnAddress(),
         creator_unknown, pAdapter, createFlags);
  PFN_D3D11_CORE_CREATE_DEVICE coreFunc = d3d11hooks.CoreCreateDevice();

  if(coreFunc == NULL)
  {
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if(d3d11)
      coreFunc = (PFN_D3D11_CORE_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CoreCreateDevice");
  }

  if(coreFunc == NULL)
  {
    RDCERR("D3D11CoreCreateDevice onward pointer unavailable");
    return E_UNEXPECTED;
  }

  static LONG g_coreWrapCount = 0;
  LONG coreWrapIndex = InterlockedIncrement(&g_coreWrapCount);
  RDCLOG("RDOC_DIAG ENTRY_CORE_INDEX index=%ld action=%s", coreWrapIndex,
         coreWrapIndex == 1 ? "wrap" : "passthrough");
  if(coreWrapIndex > 1)
    return coreFunc(creator_unknown, pAdapter, unk3_zero, arg4_unknown, createFlags,
                    pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel);

  g_coreReal = coreFunc;
  return d3d11hooks.Create_Internal(&D3D11CoreRealBridge, (IDXGIAdapter *)pAdapter,
                                    D3D_DRIVER_TYPE_HARDWARE, NULL, createFlags & ~1u,
                                    pFeatureLevels, FeatureLevels, SDKVersion, NULL, NULL,
                                    ppDevice, pFeatureLevel, NULL);
}

HRESULT CreateD3D11_Internal(RealD3D11CreateFunction real, __in_opt IDXGIAdapter *pAdapter,
                             D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                             __in_ecount_opt(FeatureLevels) CONST D3D_FEATURE_LEVEL *pFeatureLevels,
                             UINT FeatureLevels, UINT SDKVersion,
                             __in_opt CONST DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                             __out_opt IDXGISwapChain **ppSwapChain,
                             __out_opt ID3D11Device **ppDevice,
                             __out_opt D3D_FEATURE_LEVEL *pFeatureLevel,
                             __out_opt ID3D11DeviceContext **ppImmediateContext)
{
  return D3D11Hook::d3d11hooks.Create_Internal(
      real, pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
      pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
}

volatile void *D3D11Hook::INTERNAL_Keep[4] = {
    (void *)D3D11CreateDevice_hook,
    (void *)D3D11CreateDeviceAndSwapChain_hook,
    (void *)D3D11CoreCreateDevice_hook,
    (void *)D3D11CreateDeviceAndSwapChainImpl_hook,
};
