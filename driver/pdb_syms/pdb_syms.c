// pdb_syms.c - extract wrapper RVAs and orig-store offsets from rendertest.pdb
// build: cl /nologo /W4 /O2 pdb_syms.c /Fe:pdb_syms.exe dbghelp.lib
// usage: pdb_syms.exe <rendertest.dll> <rendertest.pdb>
//
// emits machine-parseable lines:
//   SYM <name> RVA 0x<hex>
//   OBJ <instance> RVA 0x<hex>
//   MEMBER <instance> <member> OFFSET 0x<hex>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <dbghelp.h>

typedef struct _CFG {
    DWORD64 modBase;
    FILE *out;
    DWORD64 objRva[4];
    int objCount;
} CFG;

static const char *k_WantFuncs[] = {
    "DXGIHook::CreateDXGIFactory_hook",
    "DXGIHook::CreateDXGIFactory1_hook",
    "DXGIHook::CreateDXGIFactory2_hook",
    "DXGIHook::DXGIGetDebugInterface1_hook",
    "D3D11Hook::D3D11CreateDevice_hook",
    "D3D11Hook::D3D11CreateDeviceAndSwapChain_hook",
    "D3D11Hook::D3D11CoreCreateDevice_hook",
    "D3D11Hook::D3D11CreateDeviceAndSwapChainImpl_hook",
    "D3D12Hook::D3D12CreateDevice_hook",
    "D3D12Hook::D3D12GetInterface_hook",
    NULL,
};

static const char *k_WantObjs[] = {
    "DXGIHook::dxgihooks",
    "D3D12Hook::d3d12hooks",
    "D3D11Hook::d3d11hooks",
    NULL,
};

static const char *k_WantMembers[] = {
    "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2",
    "GetDebugInterface1", "CreateDevice", "CreateDeviceAndSwapChain",
    "GetInterface", "CoreCreateDevice",
    NULL,
};

static void DumpMembers(HANDLE hp, DWORD64 modBase, DWORD typeIndex, const char *instance)
{
    TI_FINDCHILDREN_PARAMS *fc = NULL;
    DWORD count = 0;
    if (!SymGetTypeInfo(hp, modBase, typeIndex, TI_GET_CHILDRENCOUNT, &count) || count == 0)
        return;
    fc = (TI_FINDCHILDREN_PARAMS *)calloc(1, sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    fc->Count = count;
    fc->Start = 0;
    if (!SymGetTypeInfo(hp, modBase, typeIndex, TI_FINDCHILDREN, fc)) {
        free(fc);
        return;
    }
    for (DWORD i = 0; i < fc->Start + (count ? min(count, fc->Count) : 0); i++) {
        DWORD child = fc->ChildId[i];
        WCHAR *wname = NULL;
        if (!SymGetTypeInfo(hp, modBase, child, TI_GET_SYMNAME, &wname) || !wname)
            continue;
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), NULL, NULL);
        LocalFree(wname);
        for (int m = 0; k_WantMembers[m]; m++) {
            if (!strcmp(name, k_WantMembers[m])) {
                DWORD offset = 0;
                if (SymGetTypeInfo(hp, modBase, child, TI_GET_OFFSET, &offset))
                    printf("MEMBER %s %s OFFSET 0x%x\n", instance, name, offset);
            }
        }
    }
    free(fc);
}

static BOOL CALLBACK EnumSym(PSYMBOL_INFOW sym, ULONG size, PVOID ctx)
{
    UNREFERENCED_PARAMETER(size);
    CFG *cfg = (CFG *)ctx;
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, sym->Name, -1, name, sizeof(name), NULL, NULL);

    for (int i = 0; k_WantFuncs[i]; i++) {
        if (!strcmp(name, k_WantFuncs[i]))
            printf("SYM %s RVA 0x%llx\n", name, (unsigned long long)(sym->Address - cfg->modBase));
    }
    for (int i = 0; k_WantObjs[i]; i++) {
        if (!strcmp(name, k_WantObjs[i])) {
            printf("OBJ %s RVA 0x%llx\n", name, (unsigned long long)(sym->Address - cfg->modBase));
            DumpMembers(GetCurrentProcess(), cfg->modBase, sym->TypeIndex, k_WantObjs[i] + strlen("DXGIHook::") - 0);
        }
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: pdb_syms.exe <rendertest.dll> <rendertest.pdb>\n");
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_ANYTHING);
    if (!SymInitialize(GetCurrentProcess(), NULL, FALSE)) {
        printf("SymInitialize failed %lu\n", GetLastError());
        return 1;
    }
    // point symbol search at the pdb's directory
    char searchPath[MAX_PATH * 2];
    char pdbDir[MAX_PATH];
    strncpy(pdbDir, argv[2], sizeof(pdbDir) - 1);
    pdbDir[sizeof(pdbDir) - 1] = 0;
    char *slash = strrchr(pdbDir, '\\');
    if (slash) *slash = 0;
    snprintf(searchPath, sizeof(searchPath), "%s;%s", pdbDir, pdbDir);
    SymSetSearchPath(GetCurrentProcess(), searchPath);

    DWORD64 base = SymLoadModuleEx(GetCurrentProcess(), NULL, argv[1], NULL, 0, 0, NULL, 0);
    if (!base) {
        printf("SymLoadModuleEx(%s) failed %lu\n", argv[1], GetLastError());
        return 1;
    }
    CFG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.modBase = base;

    printf("# module %s base 0x%llx\n", argv[1], (unsigned long long)base);
    if (!SymEnumSymbolsW(GetCurrentProcess(), base, NULL, EnumSym, &cfg)) {
        printf("SymEnumSymbols failed %lu\n", GetLastError());
        return 1;
    }
    SymUnloadModule64(GetCurrentProcess(), base);
    SymCleanup(GetCurrentProcess());
    return 0;
}
