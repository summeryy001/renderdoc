// rdoc_earlyhook.c - kernel load-time takeover for RenderDoc (phase 2 driver)
//
// TRAMPOLINE design:
//   PsSetLoadImageNotifyRoutine fires for dxgi.dll / d3d12.dll mapped into the
//   target game process before any user-mode code (incl. ACE erasure) and before
//   the loader snaps importers' IATs. In that window, in the loader thread:
//     1) resolve the watched export (realAddr)
//     2) build a trampoline = [first N whole instructions] + [mov rax, entry+N; jmp rax]
//        on a driver-allocated RWX page inside the target
//     3) orig store <- trampoline (BEFORE the detour: dxgi wrappers call orig
//        unconditionally; d3d12 wrapper's GetProcAddress fallback would recurse)
//     4) detour the entry: mov rax, wrapper; jmp rax (12 bytes, COW-private)
//
// CI test-mode spoof (safe variant):
//   ACE's pre-engine gate refuses to run under test signing. The user-mode check
//   is NtQuerySystemInformation(SystemCodeIntegrityInformation), dispatched by nt
//   through a CI-supplied callback table (a cluster of CI-range pointers in nt's
//   writable data). We match cluster members STATICALLY against CI's three
//   [NONAME] export RVAs, swap one at a time, and self-verify via our own kernel
//   query of the same class - no blind calls into CI internals.

#include <ntifs.h>
#include <ntddk.h>
#include <ntimage.h>
#include <ntstrsafe.h>
#include "rdoc_earlyhook.h"

#define RDOC_TAG 'ocdr'
#define RDOC_DEV_NAME  L"\\Device\\RdocEarlyHook"
#define RDOC_LINK_NAME L"\\??\\RdocEarlyHook"

#ifndef MEM_COMMIT
#define MEM_COMMIT  0x1000
#endif
#ifndef MEM_IMAGE
#define MEM_IMAGE   0x1000000
#endif
#ifndef MEM_PRIVATE
#define MEM_PRIVATE 0x20000
#endif
#ifndef MEM_MAPPED
#define MEM_MAPPED  0x40000
#endif

NTSYSAPI
NTSTATUS
NTAPI
ZwProtectVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG NewProtect,
    _Out_ PULONG OldProtect
);

// ---------------------------------------------------------------- v10 safety
//
// The 16:14 0x1E postmortem (dump 111326-6812-02, symbolized via PDB):
//   ControllerWork -> DoArm -> DisarmLocked (stale, DEAD target) ->
//   KeStackAttachProcess(dead eproc) -> RevertEntry -> ReadUser(realAddr of a
//   torn-down address space) -> AV -> 0x1E.
// ReadUser HAS __try/__except - but it never fired: a manually mapped (kdmapper)
// image is invisible to RtlLookupFunctionTable, so kernel exception unwind
// cannot find any RUNTIME_FUNCTION in this driver -> every __try in v5 and
// earlier was decorative. RtlAddFunctionTable is NOT exported by ntoskrnl,
// so the function table cannot be registered either.
//
// Fix: never rely on SEH for user-memory access again. All user reads/writes
// go through MmCopyVirtualMemory (validates, returns error instead of
// raising), and we never attach to a dead process (TargetAlive guard).
typedef NTSTATUS (*RDOC_MM_COPY_FN)(
    _In_ PEPROCESS FromProcess,
    _In_ PVOID FromAddress,
    _In_ PEPROCESS ToProcess,
    _Out_writes_bytes_(BufferSize) PVOID ToAddress,
    _In_ SIZE_T BufferSize,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T ReturnedSize);

static RDOC_MM_COPY_FN g_mmCopy = NULL;
// PsInitialSystemProcess comes from ntddk.h

#define RDOC_MAX_DLLS 8
#define DETOUR_BYTES  12   // 48 B8 <imm64> FF E0
#define SAVE_BYTES    16

static PDEVICE_OBJECT g_devObj = NULL;
static struct _WORK_QUEUE_ITEM g_ctrlWork;
static volatile LONG g_workQueued = 0;
static VOID ControllerWork(PVOID);

// ---------------------------------------------------------------- state

typedef struct _DLL_SLOT {
    WCHAR   name[32];        // watched basename (lowercase)
    UINT64  mapBase;         // base of the loader-mapped image in target
    UINT64  mapSize;
    HANDLE  section;
    HANDLE  file;
    BOOLEAN seen;
} DLL_SLOT;

typedef struct _ENTRY_STATE {
    RDOC_ARM_ENTRY cfg;
    UINT64 realAddr;         // export entry in the loaded watched image
    UINT64 origAddr;         // trampoline address inside the target
    UCHAR  origBytes[SAVE_BYTES];
    UCHAR  tramp[SAVE_BYTES + 16];
    ULONG  trampLen;
    ULONG  oldProtect;
    BOOLEAN dllSeen, exportOk, cleanOk, origFilled, detoured, iatOk;
    LONG64 scanHits;
} ENTRY_STATE;

C_ASSERT(sizeof(((ENTRY_STATE *)0)->tramp) >= SAVE_BYTES + 14);

static RDOC_ARM_IN   g_cfg;
static ENTRY_STATE   g_entries[RDOC_MAX_ENTRIES];
static DLL_SLOT      g_dlls[RDOC_MAX_DLLS];
static ULONG         g_dllCount;
static BOOLEAN       g_armed = FALSE;
static PEPROCESS     g_eproc = NULL;
static UINT64        g_targetPid = 0;
static UINT64        g_trampAddr = 0;     // trampoline page in target (32B per entry)
static EX_PUSH_LOCK  g_lock;
static volatile LONG g_mapDepth = 0;
// v10 modscan: bases of loader-mapped images (PEB LDR walk at arm time).
// A PE in the VAS that is NOT in this list = manually mapped, even when the
// memory manager reports MEM_IMAGE (SEC_IMAGE section views are MEM_IMAGE
// too - proven by modscan_sim).
#define RDOC_MAX_LDR 512
static UINT64        g_ldrBases[RDOC_MAX_LDR];
static ULONG         g_ldrBaseCount = 0;

static BOOLEAN InLdrList(_In_ UINT64 base)
{
    for (ULONG i = 0; i < g_ldrBaseCount; i++)
        if (g_ldrBases[i] == base)
            return TRUE;
    return FALSE;
}

// ---------------------------------------------------------------- ring log

static CHAR  g_log[RDOC_LOG_SIZE];
static ULONG g_logLen = 0;
static KSPIN_LOCK g_logLock;

static VOID Logf(_In_z_ _Printf_format_string_ PCSTR fmt, ...)
{
    CHAR buf[384];
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS st = RtlStringCbVPrintfA(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (!NT_SUCCESS(st))
        return;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[rdoc_earlyhook] %s\n", buf);
    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    ULONG len = (ULONG)strlen(buf);
    if (g_logLen + len + 2 < RDOC_LOG_SIZE) {
        RtlCopyMemory(g_log + g_logLen, buf, len);
        g_logLen += len;
        g_log[g_logLen++] = '\n';
        g_log[g_logLen] = '\0';
    }
    KeReleaseSpinLock(&g_logLock, irql);
}

// ---------------------------------------------------------------- helpers

static BOOLEAN WciEquals(_In_z_ const WCHAR *a, _In_z_ const WCHAR *b)
{
    while (*a && *b) {
        WCHAR x = *a, y = *b;
        if (x >= L'A' && x <= L'Z') x = x - L'A' + L'a';
        if (y >= L'A' && y <= L'Z') y = y - L'A' + L'a';
        if (x != y) return FALSE;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static VOID WLower(_Inout_z_ WCHAR *s)
{
    for (; *s; s++)
        if (*s >= L'A' && *s <= L'Z') *s = *s - L'A' + L'a';
}

static BOOLEAN AciEquals(_In_z_ const char *a, _In_z_ const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = x - 'A' + 'a';
        if (y >= 'A' && y <= 'Z') y = y - 'A' + 'a';
        if (x != y) return FALSE;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

// v10: when a target process is armed, all user-memory movement goes through
// MmCopyVirtualMemory against the explicit target EPROCESS - it validates the
// range (dead process / torn-down VAS / bad page => clean NTSTATUS failure,
// never a raised exception). The __try fallback path only serves contexts
// where no target is armed (boot selftest mapping into System space).
static BOOLEAN ReadUser(_In_ UINT64 addr, _Out_ PVOID dst, _In_ SIZE_T len)
{
    if (g_mmCopy && g_eproc) {
        SIZE_T got = 0;
        NTSTATUS st = g_mmCopy((PEPROCESS)g_eproc, (PVOID)addr,
                               PsInitialSystemProcess, dst, len, KernelMode, &got);
        return NT_SUCCESS(st) && got == len;
    }
    __try {
        RtlCopyMemory(dst, (const PVOID)addr, len);
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

static BOOLEAN WriteUser(_In_ UINT64 addr, _In_ const PVOID src, _In_ SIZE_T len)
{
    if (g_mmCopy && g_eproc) {
        SIZE_T got = 0;
        NTSTATUS st = g_mmCopy(PsInitialSystemProcess, (PVOID)src,
                               (PEPROCESS)g_eproc, (PVOID)addr, len, KernelMode, &got);
        return NT_SUCCESS(st) && got == len;
    }
    __try {
        RtlCopyMemory((PVOID)addr, src, len);
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

// is the armed target still alive (same EPROCESS for the pid)? cheap pid probe
static BOOLEAN TargetAlive(VOID)
{
    if (!g_eproc || !g_targetPid)
        return FALSE;
    PEPROCESS live = NULL;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)g_targetPid, &live);
    if (!NT_SUCCESS(st))
        return FALSE;
    BOOLEAN same = (live == (PEPROCESS)g_eproc);
    ObDereferenceObject(live);
    return same;
}

// ---------------------------------------------------------------- PE export lookup
//
// v10: every field read goes through ReadUser (MmCopyVirtualMemory when a
// target is armed). Direct derefs from the system worker thread were the v4
// 0x1E class, and the __try wrappers are decorative in a mapped image.

static BOOLEAN ReadU16(_In_ UINT64 addr, _Out_ PUSHORT v)  { return ReadUser(addr, v, sizeof(USHORT)); }
static BOOLEAN ReadU32(_In_ UINT64 addr, _Out_ PULONG v)   { return ReadUser(addr, v, sizeof(ULONG)); }

static UINT64 FindExport(_In_ UINT64 imageBase, _In_z_ const char *funcName)
{
    LONG   e_lfanew = 0;
    UINT32 sig = 0;
    USHORT magic = 0;

    USHORT dosMagic = 0;
    if (!ReadU16(imageBase, &dosMagic) || dosMagic != IMAGE_DOS_SIGNATURE)
        return 0;
    if (!ReadU32(imageBase + 0x3C, (PULONG)&e_lfanew))
        return 0;
    if (e_lfanew <= 0 || e_lfanew > 0x400)
        return 0;

    UINT64 nt = imageBase + (UINT64)e_lfanew;
    if (!ReadU32(nt, &sig) || sig != IMAGE_NT_SIGNATURE)
        return 0;
    if (!ReadU16(nt + 0x18, &magic) || magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 0;

    // DataDirectory[0] = export table, at OptionalHeader+0x70 => nt+0x88
    ULONG dirRva = 0, dirSize = 0;
    if (!ReadU32(nt + 0x88, &dirRva) || !ReadU32(nt + 0x8C, &dirSize))
        return 0;
    if (dirRva == 0 || dirSize == 0)
        return 0;

    // IMAGE_EXPORT_DIRECTORY fields we need: NumberOfNames @+0x18,
    // AddressOfFunctions @+0x1C, AddressOfNames @+0x20, AddressOfNameOrdinals @+0x24
    UINT64 expDir = imageBase + dirRva;
    ULONG  numberOfNames = 0, addrFuncs = 0, addrNames = 0, addrOrds = 0;
    if (!ReadU32(expDir + 0x18, &numberOfNames) ||
        !ReadU32(expDir + 0x1C, &addrFuncs) ||
        !ReadU32(expDir + 0x20, &addrNames) ||
        !ReadU32(expDir + 0x24, &addrOrds))
        return 0;
    if (numberOfNames == 0 || numberOfNames > 0x10000)
        return 0;

    for (ULONG i = 0; i < numberOfNames; i++) {
        ULONG nameRva = 0;
        if (!ReadU32(imageBase + addrNames + (UINT64)i * 4, &nameRva))
            return 0;
        CHAR n[64];
        RtlZeroMemory(n, sizeof(n));
        if (!ReadUser(imageBase + nameRva, n, sizeof(n) - 1))
            return 0;
        if (AciEquals(n, funcName)) {
            USHORT ord = 0;
            if (!ReadU16(imageBase + addrOrds + (UINT64)i * 2, &ord))
                return 0;
            ULONG funcRva = 0;
            if (!ReadU32(imageBase + addrFuncs + (UINT64)ord * 4, &funcRva))
                return 0;
            if (funcRva >= dirRva && funcRva < dirRva + dirSize)
                return 0;               // forwarded export
            return imageBase + funcRva;
        }
    }
    return 0;
}

static UINT64 ImageSizeOf(_In_ UINT64 imageBase)
{
    USHORT dosMagic = 0;
    LONG   e_lfanew = 0;
    UINT32 sig = 0;
    USHORT magic = 0;
    ULONG  sizeOfImage = 0;

    if (!ReadU16(imageBase, &dosMagic) || dosMagic != IMAGE_DOS_SIGNATURE)
        return 0;
    if (!ReadU32(imageBase + 0x3C, (PULONG)&e_lfanew))
        return 0;
    if (e_lfanew <= 0 || e_lfanew > 0x400)
        return 0;
    UINT64 nt = imageBase + (UINT64)e_lfanew;
    if (!ReadU32(nt, &sig) || sig != IMAGE_NT_SIGNATURE)
        return 0;
    if (!ReadU16(nt + 0x18, &magic) || magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 0;
    // SizeOfImage at OptionalHeader+0x38 => nt+0x50
    if (!ReadU32(nt + 0x50, &sizeOfImage))
        return 0;
    return sizeOfImage;
}

// ---------------------------------------------------------------- image copy mapping (selftest path)

static BOOLEAN MapDllCopy(_In_z_ const WCHAR *displayName, _In_z_ const WCHAR *ntPath,
                          _Out_ UINT64 *outBase, _Out_ UINT64 *outSize,
                          _Out_ HANDLE *outSec, _Out_ HANDLE *outFile)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st;
    HANDLE file = NULL, sec = NULL;

    RtlInitUnicodeString(&path, ntPath);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    st = ZwCreateFile(&file, GENERIC_READ | GENERIC_EXECUTE | SYNCHRONIZE, &oa, &iosb, NULL,
                      FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) {
        Logf("clean: ZwCreateFile(%ws) failed 0x%08X", displayName, st);
        return FALSE;
    }

    st = ZwCreateSection(&sec, SECTION_MAP_READ | SECTION_MAP_EXECUTE, NULL, NULL,
                         PAGE_EXECUTE_READWRITE, SEC_IMAGE, file);
    if (!NT_SUCCESS(st)) {
        Logf("clean: ZwCreateSection(%ws) failed 0x%08X", displayName, st);
        ZwClose(file);
        return FALSE;
    }

    PVOID base = NULL;
    SIZE_T viewSize = 0;
    LARGE_INTEGER secOff = {0};

    InterlockedIncrement(&g_mapDepth);
    st = ZwMapViewOfSection(sec, NtCurrentProcess(), &base, 0, 0, &secOff,
                            &viewSize, ViewShare, 0, PAGE_EXECUTE_READWRITE);
    InterlockedDecrement(&g_mapDepth);

    if (!NT_SUCCESS(st) || base == NULL) {
        Logf("clean: ZwMapViewOfSection(%ws) failed 0x%08X", displayName, st);
        ZwClose(sec); ZwClose(file);
        return FALSE;
    }

    *outBase = (UINT64)base;
    *outSize = ImageSizeOf((UINT64)base);
    *outSec = sec;
    *outFile = file;
    Logf("clean: %ws mapped at 0x%llx size 0x%llx", displayName, *outBase, *outSize);
    return TRUE;
}

static VOID BuildCleanPath(_In_ const DLL_SLOT *slot, _Out_ WCHAR *buf, _In_ ULONG bufCch)
{
    RtlStringCbPrintfW(buf, bufCch * sizeof(WCHAR), L"\\SystemRoot\\System32\\%ws", slot->name);
}

// ---------------------------------------------------------------- LDE + trampoline

static ULONG InstrLen(_In_reads_(8) const UCHAR *p)
{
    ULONG i = 0;

    while (i < 4 && (p[i] == 0x66 || p[i] == 0x67 || p[i] == 0xF2 || p[i] == 0xF3 ||
                     p[i] == 0x2E || p[i] == 0x36 || p[i] == 0x3E || p[i] == 0x26 ||
                     p[i] == 0x64 || p[i] == 0x65)) {
        if (p[i] == 0x64 || p[i] == 0x65)
            return 0;
        i++;
    }
    if (i < 4 && (p[i] & 0xF0) == 0x40)
        i++;

    UCHAR op = p[i];
    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
    case 0x56: case 0x57:
        return i + 1;
    case 0x89: case 0x8B: {
        UCHAR modrm = p[i + 1];
        UCHAR mod = modrm >> 6, rm = modrm & 7;
        ULONG len = i + 2;
        if (mod != 3 && rm == 4) {
            UCHAR sib = p[i + 2];
            len++;
            if (mod == 0 && (sib & 7) == 5)
                return 0;
        }
        if (mod == 0) {
            if (rm == 5)
                return 0;
            return len;
        }
        if (mod == 1)
            return len + 1;
        if (mod == 3)
            return len;
        return len + 4;
    }
    case 0x83: case 0x81: {
        UCHAR modrm = p[i + 1];
        UCHAR mod = modrm >> 6, rm = modrm & 7;
        ULONG imm = (op == 0x81) ? 4 : 1;
        ULONG len = i + 2;
        if (mod != 3 && rm == 4) {
            UCHAR sib = p[i + 2];
            len++;
            if (mod == 0 && (sib & 7) == 5)
                return 0;
        }
        if (mod == 0) {
            if (rm == 5)
                return 0;
            return len + imm;
        }
        if (mod == 1)
            return len + 1 + imm;
        if (mod == 2)
            return len + 4 + imm;
        return len + imm;
    }
    case 0xCC:
        return i + 1;
    case 0x90:
        return i + 1;
    default:
        if (op == 0x0F) {
            UCHAR op2 = p[i + 1];
            if (op2 == 0x1F) {
                UCHAR modrm = p[i + 2];
                UCHAR mod = modrm >> 6, rm = modrm & 7;
                ULONG len = i + 3;
                if (rm == 4) len++;
                if (mod == 1) return len + 1;
                if (mod == 2) return len + 4;
                if (mod == 0 && rm == 5) return 0;
                return len;
            }
            if (op2 == 0x1E && p[i + 2] == 0xFA)
                return i + 4;
            return 0;
        }
        return 0;
    }
}

static ULONG PrologueLen(_In_ UINT64 addr, _In_ ULONG want, _In_ ULONG maxLen)
{
    UCHAR buf[32];
    if (!ReadUser(addr, buf, sizeof(buf)))
        return 0;
    ULONG total = 0;
    while (total < want && total < maxLen) {
        ULONG n = InstrLen(buf + total);
        if (n == 0)
            return 0;
        total += n;
    }
    return (total >= want && total <= maxLen) ? total : 0;
}

static NTSTATUS g_lastMkRwSt = 0;

static BOOLEAN MakeRW(_In_ UINT64 addr, _In_ SIZE_T len, _Out_ ULONG *oldProt)
{
    PVOID b = (PVOID)addr;
    SIZE_T s = len;
    NTSTATUS st = ZwProtectVirtualMemory(NtCurrentProcess(), &b, &s, PAGE_EXECUTE_READWRITE, oldProt);
    g_lastMkRwSt = st;
    return NT_SUCCESS(st);
}

static VOID RestoreProt(_In_ UINT64 addr, _In_ SIZE_T len, _In_ ULONG prot)
{
    PVOID b = (PVOID)addr;
    SIZE_T s = len;
    ULONG old = 0;
    ZwProtectVirtualMemory(NtCurrentProcess(), &b, &s, prot, &old);
}

static BOOLEAN ApplyDetour(_In_ UINT64 entry, _In_ UINT64 wrapper)
{
    UCHAR code[DETOUR_BYTES] = { 0x48, 0xB8 };
    RtlCopyMemory(code + 2, &wrapper, 8);
    code[10] = 0xFF;
    code[11] = 0xE0;
    if (!WriteUser(entry, code, DETOUR_BYTES))
        return FALSE;
    return TRUE;
}

// v21.3: verify AFTER protection restore - the COW private copy the write
// landed in gets DISCARDED by ZwProtect back to the original (VM-proven:
// driver-side readback passed while the target still executed the original
// prologue). This check must run with the protection already restored.
static BOOLEAN DetourBytesVisible(_In_ UINT64 entry, _In_ UINT64 wrapper)
{
    UCHAR expect[DETOUR_BYTES] = { 0x48, 0xB8 };
    RtlCopyMemory(expect + 2, &wrapper, 8);
    expect[10] = 0xFF;
    expect[11] = 0xE0;
    UCHAR back[DETOUR_BYTES];
    RtlZeroMemory(back, sizeof(back));
    return ReadUser(entry, back, DETOUR_BYTES) &&
           RtlCompareMemory(back, expect, DETOUR_BYTES) == DETOUR_BYTES;
}

static BOOLEAN DetourEntry(_Inout_ ENTRY_STATE *e)
{
    UCHAR cur[SAVE_BYTES];

    if (!ReadUser(e->realAddr, cur, SAVE_BYTES)) {
        Logf("detour: read entry 0x%llx failed", e->realAddr);
        return FALSE;
    }
    RtlCopyMemory(e->origBytes, cur, SAVE_BYTES);

    ULONG oldProt = 0;
    if (!MakeRW(e->realAddr, SAVE_BYTES, &oldProt)) {
        Logf("detour: MakeRW(0x%llx) failed st=0x%08X", e->realAddr, g_lastMkRwSt);
        return FALSE;
    }
    BOOLEAN ok = ApplyDetour(e->realAddr, e->cfg.wrapperAbs);
    RestoreProt(e->realAddr, SAVE_BYTES, oldProt);
    e->oldProtect = oldProt;

    // v21.3: COW-discard guard - verify with the ORIGINAL protection restored;
    // if the bytes vanished (private copy discarded), re-open RW, direct-copy
    // and LEAVE the page RW so the modification survives.
    if (ok && !DetourBytesVisible(e->realAddr, e->cfg.wrapperAbs)) {
        Logf("detour: COW_DISCARD at 0x%llx - retrying, page stays RW", e->realAddr);
        ULONG oldProt2 = 0;
        if (MakeRW(e->realAddr, SAVE_BYTES, &oldProt2)) {
            UCHAR code2[DETOUR_BYTES] = { 0x48, 0xB8 };
            RtlCopyMemory(code2 + 2, &e->cfg.wrapperAbs, 8);
            code2[10] = 0xFF;
            code2[11] = 0xE0;
            RtlCopyMemory((PVOID)e->realAddr, code2, DETOUR_BYTES);
            if (DetourBytesVisible(e->realAddr, e->cfg.wrapperAbs)) {
                Logf("detour: COW retry LANDED (page left RW)");
                e->oldProtect = oldProt2;    // keep RW: restoring would discard again
            } else {
                Logf("detour: COW retry still not visible");
                ok = FALSE;
            }
        } else {
            ok = FALSE;
        }
    }

    if (ok) {
        e->detoured = TRUE;
        Logf("detour: %ws!%s 0x%llx -> wrapper 0x%llx OK",
             e->cfg.dllName, e->cfg.funcName, e->realAddr, e->cfg.wrapperAbs);
    } else {
        Logf("detour: write 0x%llx failed", e->realAddr);
    }
    return ok;
}

static VOID RevertEntry(_Inout_ ENTRY_STATE *e)
{
    if (!e->detoured)
        return;
    UCHAR cur[SAVE_BYTES];
    if (ReadUser(e->realAddr, cur, SAVE_BYTES) && cur[0] == 0x48 && cur[1] == 0xB8) {
        ULONG oldProt = 0;
        if (MakeRW(e->realAddr, SAVE_BYTES, &oldProt)) {
            WriteUser(e->realAddr, e->origBytes, SAVE_BYTES);
            RestoreProt(e->realAddr, SAVE_BYTES, oldProt);
        }
    }
    e->detoured = FALSE;
    Logf("revert: %ws!%s restored", e->cfg.dllName, e->cfg.funcName);
}

// ---------------------------------------------------------------- load image notify

// v10: arm one entry against an arbitrary image base. Shared by the
// load-image notify path AND the VAS modscan (which feeds it manually mapped
// images that never fire the notify callback - the invisible D3D12Core hunt).
// Caller holds g_lock. Returns TRUE when the entry got detoured here.
static BOOLEAN TryArmEntryOnBase(_Inout_ ENTRY_STATE *e, _In_ UINT64 base, _In_z_ const WCHAR *via)
{
    if (e->exportOk)
        return FALSE;                        // already armed on another image

    UINT64 real = 0;
    if (_strnicmp(e->cfg.funcName, "@rva:", 5) == 0) {
        ULONG rva = 0;
        NTSTATUS rvaStatus = RtlCharToInteger(e->cfg.funcName + 5, 16, &rva);
        if (!NT_SUCCESS(rvaStatus) || rva == 0) {
            Logf("watch: invalid RVA target %s", e->cfg.funcName);
            return FALSE;
        }
        real = base + rva;
    } else {
        real = FindExport(base, e->cfg.funcName);
    }
    if (real == 0)
        return FALSE;
    e->realAddr = real;
    e->dllSeen = TRUE;
    e->exportOk = TRUE;
    Logf("watch: %ws!%s real=0x%llx (via %ws)",
         e->cfg.dllName, e->cfg.funcName, real, via);

    if (!g_trampAddr) {
        Logf("watch: no trampoline page - entry %s deferred", e->cfg.funcName);
        return FALSE;
    }

    ULONG n = PrologueLen(e->realAddr, DETOUR_BYTES, SAVE_BYTES);
    if (n == 0) {
        Logf("tramp: %s prologue has unknown instruction - SKIPPED", e->cfg.funcName);
        return FALSE;
    }
    UCHAR tramp[SAVE_BYTES + 16];
    if (!ReadUser(e->realAddr, tramp, n)) {
        Logf("tramp: read prologue of %s failed", e->cfg.funcName);
        return FALSE;
    }
    // v12 trampoline tail: push imm32(lo); mov [rsp+4], imm32(hi); ret
    //   68 XX XX XX XX  C7 44 24 04 XX XX XX XX  C3     = 14 bytes
    // ZERO register clobber. The v10 tail (mov rax, back; jmp rax) destroyed
    // RAX, and d3d11!D3D11CreateDeviceAndSwapChain reads RAX AFTER the cut
    // (+0x00 mov rax,rsp inside the stolen window, +0x18 lea rbp,[rax-0x2F]
    // after it) -> RBP landed in the d3d11 code page -> write AV at
    // CreateDevice+0x147 (UE4CC 5B02702F, byte-exact match). The push/ret
    // tail only touches the stack: net RSP unchanged, wrapper return address
    // intact, every register preserved across the boundary.
    UINT64 back = e->realAddr + n;
    ULONG  backLo = (ULONG)(back & 0xffffffffull);
    ULONG  backHi = (ULONG)(back >> 32);
    tramp[n] = 0x68;
    RtlCopyMemory(tramp + n + 1, &backLo, 4);
    tramp[n + 5] = 0xC7; tramp[n + 6] = 0x44; tramp[n + 7] = 0x24; tramp[n + 8] = 0x04;
    RtlCopyMemory(tramp + n + 9, &backHi, 4);
    tramp[n + 13] = 0xC3;
    // n <= SAVE_BYTES(16) => n+14 <= 30 <= 32-byte slot

    UINT64 trampAddr = g_trampAddr + (UINT64)(e - g_entries) * 32;
    if (!WriteUser(trampAddr, tramp, n + 14)) {
        Logf("tramp: write for %s failed", e->cfg.funcName);
        return FALSE;
    }
    e->origAddr = trampAddr;
    e->trampLen = n + 14;
    RtlCopyMemory(e->tramp, tramp, n + 14);
    e->cleanOk = TRUE;

    if ((g_cfg.flags & RDOC_FLAG_ORIGFILL) && e->cfg.origStoreAbs) {
        UINT64 orig = e->origAddr;
        if (WriteUser(e->cfg.origStoreAbs, &orig, sizeof(orig))) {
            e->origFilled = TRUE;
            e->iatOk = TRUE;
            Logf("orig: %s store 0x%llx = trampoline 0x%llx (len %u)",
                 e->cfg.funcName, e->cfg.origStoreAbs, e->origAddr, e->trampLen);
        } else {
            Logf("orig: write store 0x%llx FAILED", e->cfg.origStoreAbs);
        }
    }

    if (g_cfg.flags & RDOC_FLAG_DETOUR) {
        if (e->origFilled || !e->cfg.origStoreAbs) {
            if (DetourEntry(e))
                return TRUE;
        } else {
            Logf("detour: skipped %s (orig path unsafe)", e->cfg.funcName);
        }
    }
    return FALSE;
}

static VOID ProcessWatchedDll(_In_ DLL_SLOT *slot)
{
    for (ULONG i = 0; i < g_cfg.entryCount; i++) {
        ENTRY_STATE *e = &g_entries[i];
        if (e->dllSeen)
            continue;
        if (!WciEquals(e->cfg.dllName, slot->name))
            continue;
        TryArmEntryOnBase(e, slot->mapBase, slot->name);
    }
}

static VOID LoadImageNotify(_In_opt_ PUNICODE_STRING FullImageName,
                            _In_ HANDLE ProcessId,
                            _In_ PIMAGE_INFO ImageInfo)
{
    if (g_mapDepth > 0)
        return;
    if (!g_armed || ProcessId == NULL)
        return;
    if ((UINT64)(ULONG_PTR)ProcessId != g_targetPid)
        return;
    if (!FullImageName || !FullImageName->Buffer || !ImageInfo || !ImageInfo->ImageBase)
        return;

    WCHAR basename[32];
    USHORT n = FullImageName->Length / sizeof(WCHAR);
    LONG s = n - 1;
    while (s >= 0 && FullImageName->Buffer[s] != L'\\' && FullImageName->Buffer[s] != L'/')
        s--;
    LONG len = n - s - 1;
    if (len <= 0 || len >= 32)
        return;
    for (LONG i = 0; i < len; i++)
        basename[i] = FullImageName->Buffer[s + 1 + i];
    basename[len] = 0;
    WLower(basename);

    ExAcquirePushLockExclusive(&g_lock);

    for (ULONG i = 0; i < g_dllCount; i++) {
        if (g_dlls[i].seen || !WciEquals(g_dlls[i].name, basename))
            continue;
        g_dlls[i].mapBase = (UINT64)ImageInfo->ImageBase;
        g_dlls[i].mapSize = (UINT64)ImageInfo->ImageSize;
        g_dlls[i].seen = TRUE;
        Logf("notify: %ws mapped in pid %llu base 0x%llx",
             basename, g_targetPid, g_dlls[i].mapBase);
        ProcessWatchedDll(&g_dlls[i]);
        // landing gap fix: queue an immediate detour retry - the worker timer
        // would only fire up to 500ms later, and the engine calls the factory
        // within ~1s of dxgi mapping (proven by round v3: detour at +6s, call
        // before that, API stayed none)
        if (InterlockedCompareExchange(&g_workQueued, 1, 0) == 0)
            ExQueueWorkItem(&g_ctrlWork, CriticalWorkQueue);
    }

    ExReleasePushLockExclusive(&g_lock);
}

// ---------------------------------------------------------------- process notify

static VOID ProcessNotify(_In_ HANDLE ParentId, _In_ HANDLE ProcessId, _In_ BOOLEAN Create)
{
    UNREFERENCED_PARAMETER(ParentId);
    if (Create)
        return;
    if (!g_armed || (UINT64)(ULONG_PTR)ProcessId != g_targetPid)
        return;

    ExAcquirePushLockExclusive(&g_lock);
    Logf("proc: target pid %llu exited - disarming (no memory restore)", g_targetPid);
    g_armed = FALSE;
    for (ULONG i = 0; i < g_dllCount; i++) {
        if (g_dlls[i].section) { ZwClose(g_dlls[i].section); g_dlls[i].section = NULL; }
        if (g_dlls[i].file)    { ZwClose(g_dlls[i].file);    g_dlls[i].file = NULL; }
    }
    if (g_eproc) {
        ObDereferenceObject(g_eproc);
        g_eproc = NULL;
    }
    ExReleasePushLockExclusive(&g_lock);
}

// ---------------------------------------------------------------- rescan

static VOID ScanRegion(_In_ PUCHAR page, _In_ UINT64 base, _In_ UINT64 size,
                       _Out_ PRDOC_RESCAN_OUT out)
{
    for (UINT64 p = base; p < base + size; p += 0x1000) {
        UINT64 end = p + 0x1000;
        if (end > base + size) end = base + size;
        UINT64 pgSize = end - p;

        // v10: MmCopy-based read (validates; the old __try copy was
        // decorative in a mapped image)
        if (!ReadUser(p, page, (SIZE_T)pgSize))
            continue;

        for (UINT64 o = 0; o + 8 <= pgSize; o += 8) {
            UINT64 v;
            RtlCopyMemory(&v, page + o, 8);
            for (ULONG e = 0; e < g_cfg.entryCount; e++) {
                ENTRY_STATE *es = &g_entries[e];
                if (!es->exportOk || es->realAddr == 0)
                    continue;
                if (v != es->realAddr)
                    continue;
                UINT64 cell = p + o;
                if (es->cfg.origStoreAbs && cell == es->cfg.origStoreAbs)
                    continue;
                UINT64 w = es->cfg.wrapperAbs;
                if (WriteUser(cell, &w, 8)) {
                    es->scanHits++;
                    out->totalHits++;
                    out->hitsPerEntry[e]++;
                    RtlCopyMemory(page + o, &w, 8);
                }
            }
        }
        out->bytesScanned += pgSize;
    }
}

static VOID RescanAttached(_In_ PRDOC_RESCAN_IN in, _Out_ PRDOC_RESCAN_OUT out)
{
    PUCHAR page = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 0x1000, RDOC_TAG);
    if (!page) {
        Logf("rescan: alloc failed");
        return;
    }
    out->bytesScanned = 0;
    out->totalHits = 0;

    if (in->rangeCount > 0) {
        for (ULONG r = 0; r < in->rangeCount; r++) {
            UINT64 base = in->ranges[r].base & ~7ull;
            UINT64 size = in->ranges[r].size;
            ScanRegion(page, base, size, out);
        }
    } else {
        UINT64 addr = 0x10000;
        while (addr < 0x00007FFFFFFEFFFFull) {
            MEMORY_BASIC_INFORMATION mbi;
            SIZE_T got = 0;
            NTSTATUS st = ZwQueryVirtualMemory(NtCurrentProcess(), (PVOID)addr,
                                               MemoryBasicInformation, &mbi, sizeof(mbi), &got);
            if (!NT_SUCCESS(st) || got < sizeof(mbi))
                break;
            UINT64 regionEnd = (UINT64)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
                (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
                 mbi.Protect == PAGE_EXECUTE_READWRITE || mbi.Protect == PAGE_EXECUTE_WRITECOPY)) {
                ScanRegion(page, (UINT64)mbi.BaseAddress, (UINT64)mbi.RegionSize, out);
            }
            addr = regionEnd;
        }
    }
    ExFreePoolWithTag(page, RDOC_TAG);
    Logf("rescan: %llu bytes, %lld hits", out->bytesScanned, out->totalHits);
}

// ---------------------------------------------------------------- disarm

static VOID DisarmLocked(VOID)
{
    // v10: attaching to a torn-down address space froze/faulted the system
    // twice (v4 freeze, 16:14 0x1E). If the target is gone, its pages belong
    // to the dead VAS - skip every memory operation, reclaim state only.
    BOOLEAN alive = TargetAlive();
    if (g_eproc && alive) {
        KAPC_STATE apc;
        KeStackAttachProcess((PRKPROCESS)g_eproc, &apc);
        for (ULONG i = 0; i < g_cfg.entryCount; i++)
            RevertEntry(&g_entries[i]);
        if (g_trampAddr) {
            PVOID b = (PVOID)g_trampAddr;
            SIZE_T sz = 0;
            ZwFreeVirtualMemory(NtCurrentProcess(), &b, &sz, MEM_RELEASE);
            g_trampAddr = 0;
        }
        KeUnstackDetachProcess(&apc);
    } else {
        if (g_eproc)
            Logf("disarm: target %llu gone - memory restore SKIPPED (dead VAS)",
                 g_targetPid);
        g_trampAddr = 0;
    }
    for (ULONG i = 0; i < g_dllCount; i++) {
        if (g_dlls[i].section) { ZwClose(g_dlls[i].section); g_dlls[i].section = NULL; }
        if (g_dlls[i].file)    { ZwClose(g_dlls[i].file);    g_dlls[i].file = NULL; }
    }
    if (g_eproc) {
        ObDereferenceObject(g_eproc);
        g_eproc = NULL;
    }
    g_armed = FALSE;
    g_dllCount = 0;
    Logf("disarm: complete");
}

// ---------------------------------------------------------------- file-mode control plane
//
// kdmapper-mapped drivers cannot call IoCreateDevice (the passed "DriverObject"
// is (imagebase, string) -> IoCreateDevice derefs garbage -> 0x50 BSOD, proven
// by dump). Control plane therefore runs over files:
//   C:\rdoc_arm.bin    chain writes an RDOC_ARM_IN with magic RDA1 + a fresh
//                      sequence number; the controller thread arms on it and
//                      bumps the ack sequence
//   C:\rdoc_status.bin driver writes { ack, armed, entries[] } for the chain

#define RDOC_ARM_MAGIC 0x31314452u     // 'RDA1'

typedef struct _RDOC_ARM_FILE {
    ULONG magic;
    ULONG seq;                         // chain increments to request an arm
    ULONG ackSeq;                      // driver copies seq here when consumed
    ULONG doRescan;                    // chain sets 1 to request a rescan pass
    ULONG doModscan;                   // v10: 1 = full VAS PE hunt + auto-arm
    ULONG doDisarm;                    // v10: 1 = disarm (chain failed round)
    RDOC_ARM_IN in;
} RDOC_ARM_FILE, *PRDOC_ARM_FILE;

// v21 identity/liveness tail. Appended AFTER the legacy 792-byte layout so
// every legacy reader (chain_kernel.ps1, old evidence_check.py) keeps parsing
// the front unchanged. Readers detect the tail by total length (792+36) and
// the 'RDT2' magic. Resolves the two protocol blind spots the offline checker
// called out: no build/instance nonce and no heartbeat.
//   instanceNonce : random per-load value, 0 forbidden - two snapshots with
//                   different nonces prove a remap happened between them
//   loadStamp     : KeQueryInterruptTime() at DriverEntry (100ns since boot)
//   heartbeat     : KeQueryInterruptTime() at the last status write - must
//                   advance while the driver instance is alive
//   statusWrites  : monotonic per-instance write counter - diffing two live
//                   snapshots must show it increasing
#define RDOC_TAIL_MAGIC   0x32544452u   // 'RDT2'
#define RDOC_TAIL_VERSION 21
#define RDOC_TAIL_PERIOD_5S (5ull * 1000 * 1000 * 10)   // 5s in 100ns units

typedef struct _RDOC_STATUS_TAIL_V21 {
    ULONG magic;            // 'RDT2'
    ULONG version;          // 21
    ULONG instanceNonce;    // per-load random value (never 0)
    ULONG loadStampLo;      // KeQueryInterruptTime() at DriverEntry
    ULONG loadStampHi;
    ULONG heartbeatLo;      // KeQueryInterruptTime() at last write
    ULONG heartbeatHi;
    ULONG statusWrites;     // per-instance monotonic write count
    ULONG tailChecksum;     // sum of the 8 ULONGs above (wrapping)
} RDOC_STATUS_TAIL_V21, *PRDOC_STATUS_TAIL_V21;

typedef struct _RDOC_STATUS_FILE {
    ULONG magic;                       // 'RDS1'
    ULONG ackSeq;
    ULONG armed;
    ULONG entryCount;
    ULONG selfTestResult;              // 0 = dxgi selftest passed at boot
    RDOC_STATUS_ENTRY entries[RDOC_MAX_ENTRIES];
    CHAR logTail[512];                 // last bytes of the ring log for diagnostics
    RDOC_STATUS_TAIL_V21 tail;         // v21: identity/liveness tail
} RDOC_STATUS_FILE, *PRDOC_STATUS_FILE;

static RDOC_ARM_FILE g_armFile;
static RDOC_STATUS_FILE g_lastStatus;
static ULONG g_ackSeq = 0;
static ULONG g_instanceNonce = 0;
static ULONG64 g_loadStamp = 0;
static ULONG64 g_lastWriteStamp = 0;
static ULONG g_statusWrites = 0;

static VOID WriteStatusFile(_In_ PRDOC_STATUS_FILE st)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&path, L"\\??\\C:\\rdoc_status.bin");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    NTSTATUS st2 = ZwCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (NT_SUCCESS(st2)) {
        IO_STATUS_BLOCK wios;
        ZwWriteFile(h, NULL, NULL, NULL, &wios, st, (ULONG)sizeof(*st), NULL, NULL);
        ZwClose(h);
    }
}

static VOID PublishStatus(VOID)
{
    // refresh the log tail snapshot under the log lock
    CHAR tail[512];
    {
        KIRQL irql;
        KeAcquireSpinLock(&g_logLock, &irql);
        ULONG len = g_logLen;
        ULONG take = len < 511 ? len : 511;
        RtlCopyMemory(tail, g_log + (len - take), take);
        tail[take] = 0;
        KeReleaseSpinLock(&g_logLock, irql);
    }
    RDOC_STATUS_FILE st;
    RtlZeroMemory(&st, sizeof(st));
    st.magic = 0x31534452u;            // 'RDS1'
    st.ackSeq = g_ackSeq;
    st.armed = g_armed ? 1 : 0;
    st.entryCount = g_cfg.entryCount;
    for (ULONG i = 0; i < g_cfg.entryCount && i < RDOC_MAX_ENTRIES; i++) {
        ENTRY_STATE *e = &g_entries[i];
        st.entries[i].realAddr = e->realAddr;
        st.entries[i].origAddr = e->origAddr;
        st.entries[i].scanHits = e->scanHits;
        ULONG s = 0;
        if (e->dllSeen)   s |= RDOC_ES_DLLSEEN;
        if (e->exportOk)  s |= RDOC_ES_EXPORT_OK;
        if (e->cleanOk)   s |= RDOC_ES_CLEAN_OK;
        if (e->origFilled)s |= RDOC_ES_ORIGFILL;
        if (e->detoured)  s |= RDOC_ES_DETOURED;
        if (e->iatOk)     s |= RDOC_ES_IATOK;
        st.entries[i].state = s;
    }
    RtlCopyMemory(st.logTail, tail, sizeof(st.logTail));

    // v21: dedup compares ONLY the legacy front - the tail carries a fresh
    // heartbeat each call and must not force a write. Write when the legacy
    // content changed, or every 5s to prove the instance is alive.
    {
        ULONG legacyBytes = FIELD_OFFSET(RDOC_STATUS_FILE, tail);
        BOOLEAN changed =
            RtlCompareMemory(&st, &g_lastStatus, legacyBytes) != legacyBytes;
        ULONG64 now = KeQueryInterruptTime();
        BOOLEAN stale = (now - g_lastWriteStamp) >= RDOC_TAIL_PERIOD_5S;
        if (changed || stale || g_statusWrites == 0) {
            st.tail.magic = RDOC_TAIL_MAGIC;
            st.tail.version = RDOC_TAIL_VERSION;
            st.tail.instanceNonce = g_instanceNonce;
            st.tail.loadStampLo = (ULONG)(g_loadStamp & 0xffffffffull);
            st.tail.loadStampHi = (ULONG)(g_loadStamp >> 32);
            st.tail.heartbeatLo = (ULONG)(now & 0xffffffffull);
            st.tail.heartbeatHi = (ULONG)(now >> 32);
            st.tail.statusWrites = ++g_statusWrites;
            st.tail.tailChecksum = 0;
            st.tail.tailChecksum = st.tail.magic + st.tail.version +
                                   st.tail.instanceNonce + st.tail.loadStampLo +
                                   st.tail.loadStampHi + st.tail.heartbeatLo +
                                   st.tail.heartbeatHi + st.tail.statusWrites;
            g_lastWriteStamp = now;
            g_lastStatus = st;
            WriteStatusFile(&st);
        }
    }
}

// the old ARM ioctl body, driven from the file now
static VOID DoArm(_In_ PRDOC_ARM_IN in)
{
    if (in->entryCount == 0 || in->entryCount > RDOC_MAX_ENTRIES)
        return;

    ExAcquirePushLockExclusive(&g_lock);
    if (g_armed)
        DisarmLocked();
    RtlZeroMemory(g_entries, sizeof(g_entries));
    RtlZeroMemory(g_dlls, sizeof(g_dlls));
    RtlZeroMemory(&g_cfg, sizeof(g_cfg));
    RtlCopyMemory(&g_cfg, in, sizeof(RDOC_ARM_IN));
    g_targetPid = in->targetPid;

    for (ULONG i = 0; i < in->entryCount; i++) {
        g_entries[i].cfg = in->entries[i];
        WLower(g_entries[i].cfg.dllName);
    }
    g_dllCount = 0;
    for (ULONG i = 0; i < in->entryCount; i++) {
        ULONG d = 0;
        for (; d < g_dllCount; d++)
            if (WciEquals(g_dlls[d].name, g_entries[i].cfg.dllName))
                break;
        if (d == g_dllCount && g_dllCount < RDOC_MAX_DLLS) {
            RtlCopyMemory(g_dlls[g_dllCount].name, g_entries[i].cfg.dllName,
                          sizeof(g_dlls[g_dllCount].name));
            g_dllCount++;
        }
    }

    PEPROCESS proc = NULL;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)g_targetPid, &proc);
    if (!NT_SUCCESS(st)) {
        Logf("arm: lookup pid %llu failed 0x%08X", g_targetPid, st);
        ExReleasePushLockExclusive(&g_lock);
        return;
    }
    g_eproc = proc;

    {
        KAPC_STATE apc;
        KeStackAttachProcess((PRKPROCESS)g_eproc, &apc);
        PVOID trampBase = NULL;
        SIZE_T trampSize = 0x1000;
        NTSTATUS st2 = ZwAllocateVirtualMemory(NtCurrentProcess(), &trampBase, 0,
                                               &trampSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (NT_SUCCESS(st2) && trampBase)
            g_trampAddr = (UINT64)trampBase;
        KeUnstackDetachProcess(&apc);
        Logf("arm: trampoline page %s at 0x%llx",
             g_trampAddr ? "ready" : "FAILED", g_trampAddr);
    }

    g_armed = TRUE;

    // catch-up: walk the target PEB loader list for watched dlls that were
    // mapped BEFORE we armed (preloaded d3d12/D3D12Core etc.)
    // v10: every field read goes through MmCopyVirtualMemory (ReadUser) - no
    // attach, no raw user deref from a system worker thread. A torn or
    // mid-mutation list simply ends the walk cleanly.
    {
        extern PPEB PsGetProcessPeb(PEPROCESS);
        PPEB peb = PsGetProcessPeb((PEPROCESS)g_eproc);
        UINT64 ldr = 0;
        g_ldrBaseCount = 0;
        if (peb && ReadUser((UINT64)peb + 0x18, &ldr, sizeof(ldr)) && ldr) {
            UINT64 head = ldr + 0x10;          // &Ldr->InLoadOrderModuleList
            UINT64 cur = 0;
            if (ReadUser(head, &cur, sizeof(cur))) {
                ULONG walked = 0;
                while (cur && cur != head && walked++ < RDOC_MAX_LDR) {
                    UINT64 dllBase = 0, nameBuf = 0;
                    ULONG  sizeOfImage = 0;
                    USHORT nameLen = 0;
                    if (!ReadUser(cur + 0x30, &dllBase, 8) ||
                        !ReadUser(cur + 0x40, &sizeOfImage, 4) ||
                        !ReadUser(cur + 0x58, &nameLen, 2) ||
                        !ReadUser(cur + 0x60, &nameBuf, 8))
                        break;
                    if (dllBase && g_ldrBaseCount < RDOC_MAX_LDR)
                        g_ldrBases[g_ldrBaseCount++] = dllBase;
                    if (dllBase && nameBuf && nameLen > 0 && nameLen < 64) {
                        WCHAR bn[32];
                        ULONG wl = nameLen / sizeof(WCHAR);
                        if (wl < 32 && ReadUser(nameBuf, bn, wl * sizeof(WCHAR))) {
                            bn[wl] = 0;
                            WLower(bn);
                            for (ULONG d2 = 0; d2 < g_dllCount; d2++) {
                                if (!g_dlls[d2].seen && WciEquals(g_dlls[d2].name, bn)) {
                                    g_dlls[d2].mapBase = dllBase;
                                    g_dlls[d2].mapSize = (UINT64)sizeOfImage;
                                    g_dlls[d2].seen = TRUE;
                                    Logf("peb: %ws preloaded at 0x%llx - catching up",
                                         bn, dllBase);
                                    ProcessWatchedDll(&g_dlls[d2]);
                                }
                            }
                        }
                    }
                    UINT64 next = 0;
                    if (!ReadUser(cur, &next, sizeof(next)))
                        break;
                    cur = next;
                }
            }
        }
    }

    ExReleasePushLockExclusive(&g_lock);
    Logf("arm: pid %llu, %u entries, %u dlls, flags 0x%X",
         g_targetPid, in->entryCount, g_dllCount, in->flags);
}

static VOID DoRescan(VOID)
{
    RDOC_RESCAN_IN rin;
    RDOC_RESCAN_OUT rout;
    RtlZeroMemory(&rin, sizeof(rin));      // auto mode: VAS walk
    RtlZeroMemory(&rout, sizeof(rout));
    ExAcquirePushLockExclusive(&g_lock);
    if (g_armed && g_eproc && TargetAlive()) {
        KAPC_STATE apc;
        KeStackAttachProcess((PRKPROCESS)g_eproc, &apc);
        __try {
            RescanAttached(&rin, &rout);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("rescan: exception");
        }
        KeUnstackDetachProcess(&apc);
    } else if (g_armed) {
        Logf("rescan(file): target gone - skipped");
    }
    ExReleasePushLockExclusive(&g_lock);
    Logf("rescan(file): %llu bytes, %lld hits", rout.bytesScanned, rout.totalHits);
}

// ---------------------------------------------------------------- modscan (v10)
//
// API=none killer: full VAS walk of the live target. At every committed
// region base we test for an MZ/PE image. Anything that is a PE but NOT
// MEM_IMAGE is a manually mapped module (ACE-style invisible loaders) - it
// never fired PsSetLoadImageNotifyRoutine, so the notify path could never
// hook it. For each such image we try to arm every still-unarmed watched
// entry against its export table (e.g. an invisible D3D12Core clone that
// actually exports D3D12CreateDevice).
//
// Caller: PASSIVE worker, holds g_lock, attached to a LIVE target.
static VOID ModscanAttached(_Out_ ULONG *images, _Out_ ULONG *manual,
                            _Out_ ULONG *armed)
{
    *images = 0; *manual = 0; *armed = 0;
    UINT64 addr = 0x10000;
    while (addr < 0x00007FFFFFFEFFFFull) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T got = 0;
        NTSTATUS st = ZwQueryVirtualMemory(NtCurrentProcess(), (PVOID)addr,
                                           MemoryBasicInformation, &mbi, sizeof(mbi), &got);
        if (!NT_SUCCESS(st) || got < sizeof(mbi))
            break;
        UINT64 rbase = (UINT64)mbi.BaseAddress;
        UINT64 rend  = rbase + (UINT64)mbi.RegionSize;

        if (mbi.State == MEM_COMMIT) {
            USHORT mz = 0;
            if (ReadUser(rbase, &mz, 2) && mz == 0x5A4D) {
                UINT64 imgSize = ImageSizeOf(rbase);
                if (imgSize) {
                    (*images)++;
                    // manual = not loader-mapped. SEC_IMAGE section views
                    // report MEM_IMAGE too, so the only reliable test is
                    // membership in the PEB LDR module list.
                    BOOLEAN isManual = (mbi.Type != MEM_IMAGE) || !InLdrList(rbase);
                    if (isManual) {
                        (*manual)++;
                        Logf("modscan: MANUAL PE 0x%llx size 0x%llx type 0x%lx prot 0x%lx",
                             rbase, imgSize, mbi.Type, mbi.Protect);
                        // only executable layouts can host live code:
                        // MEM_IMAGE section views and MEM_PRIVATE copies yes,
                        // raw MEM_MAPPED file data (file offsets, not RVAs) no.
                        if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_PRIVATE) {
                            for (ULONG i = 0; i < g_cfg.entryCount; i++) {
                                if (TryArmEntryOnBase(&g_entries[i], rbase, L"modscan"))
                                    (*armed)++;
                            }
                        }
                    }
                }
            }
        }
        addr = rend;
    }
    Logf("modscan: %u PE images total, %u manual, %u entries armed",
         *images, *manual, *armed);
}

static VOID DoModscan(VOID)
{
    ExAcquirePushLockExclusive(&g_lock);
    if (g_armed && g_eproc) {
        if (!TargetAlive()) {
            Logf("modscan: target gone - skipped");
        } else {
            KAPC_STATE apc;
            KeStackAttachProcess((PRKPROCESS)g_eproc, &apc);
            __try {
                ULONG images = 0, manual = 0, armed = 0;
                ModscanAttached(&images, &manual, &armed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Logf("modscan: exception");
            }
            KeUnstackDetachProcess(&apc);
        }
    } else {
        Logf("modscan: not armed - skipped");
    }
    ExReleasePushLockExclusive(&g_lock);
}

static ULONG g_selfTestResult = 0xFFFF;

static VOID DoSelftestBoot(VOID)
{
    // dxgi factory selftest through the trampoline path, result -> status file
    RDOC_SELFTEST_OUT dummy;
    RtlZeroMemory(&dummy, sizeof(dummy));
    UINT64 base = 0, size = 0;
    HANDLE sec = NULL, fh = NULL;
    DLL_SLOT probe;
    RtlZeroMemory(&probe, sizeof(probe));
    wcscpy_s(probe.name, 32, L"dxgi.dll");
    WCHAR path[64];
    RtlStringCbPrintfW(path, sizeof(path), L"\\SystemRoot\\System32\\%ws", probe.name);
    if (MapDllCopy(L"dxgi.dll", path, &base, &size, &sec, &fh)) {
        UINT64 addr = FindExport(base, "CreateDXGIFactory1");
        if (addr) {
            ULONG n = PrologueLen(addr, DETOUR_BYTES, SAVE_BYTES);
            g_selfTestResult = (n >= DETOUR_BYTES) ? 0 : 1;
        } else
            g_selfTestResult = 2;
        ZwUnmapViewOfSection(NtCurrentProcess(), (PVOID)base);
        if (sec) ZwClose(sec);
        if (fh) ZwClose(fh);
    } else
        g_selfTestResult = 3;
}

static VOID ControllerThread(_In_ PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    LARGE_INTEGER delay;
    delay.QuadPart = -500 * 1000 * 10;      // 500ms

    DoSelftestBoot();
    Logf("file: controller running (selftest=%u)", g_selfTestResult);

    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &delay);

        // read the arm file
        UNICODE_STRING path;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        RtlInitUnicodeString(&path, L"\\??\\C:\\rdoc_arm.bin");
        InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        HANDLE h = NULL;
        NTSTATUS st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (NT_SUCCESS(st)) {
            RDOC_ARM_FILE f;
            RtlZeroMemory(&f, sizeof(f));
            IO_STATUS_BLOCK rios;
            st = ZwReadFile(h, NULL, NULL, NULL, &rios, &f, (ULONG)sizeof(f), NULL, NULL);
            ZwClose(h);
            if (NT_SUCCESS(st) && rios.Information >= sizeof(f) &&
                f.magic == RDOC_ARM_MAGIC && f.seq != f.ackSeq && f.seq != g_ackSeq) {
                g_armFile = f;
                DoArm(&f.in);
                g_ackSeq = f.seq;
                Logf("file: arm seq %u consumed", f.seq);
            } else if (NT_SUCCESS(st) && rios.Information >= sizeof(f) && f.doRescan) {
                DoRescan();
                f.doRescan = 0;
                // best-effort clear of the request flag
                OBJECT_ATTRIBUTES oa2;
                RtlInitUnicodeString(&path, L"\\??\\C:\\rdoc_arm.bin");
                InitializeObjectAttributes(&oa2, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                HANDLE h2 = NULL;
                NTSTATUS st2 = ZwCreateFile(&h2, GENERIC_WRITE | SYNCHRONIZE, &oa2, &iosb, NULL,
                                            FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                                            FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
                if (NT_SUCCESS(st2)) {
                    IO_STATUS_BLOCK wios;
                    ZwWriteFile(h2, NULL, NULL, NULL, &wios, &f, (ULONG)sizeof(f), NULL, NULL);
                    ZwClose(h2);
                }
            }
        }

        PublishStatus();
    }
}

// ---------------------------------------------------------------- entry (mapped-mode)
//
// Fully pool-based control plane: a periodic timer DPC queues a PASSIVE-level
// work item that polls the arm file and publishes status. No DriverObject,
// no DeviceObject, no system thread (PsCreateSystemThread is refused with
// 0xC0000022 from mapped images). DriverEntry always returns SUCCESS so
// kdmapper --free keeps the image resident.

static KTIMER g_ctrlTimer;
static KDPC g_ctrlDpc;
static VOID ControllerWork(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    InterlockedExchange(&g_workQueued, 0);

    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&path, L"\\??\\C:\\rdoc_arm.bin");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    NTSTATUS st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (NT_SUCCESS(st)) {
        RDOC_ARM_FILE f;
        RtlZeroMemory(&f, sizeof(f));
        IO_STATUS_BLOCK rios;
        st = ZwReadFile(h, NULL, NULL, NULL, &rios, &f, (ULONG)sizeof(f), NULL, NULL);
        ZwClose(h);
        if (NT_SUCCESS(st) && rios.Information >= sizeof(f) && f.magic == RDOC_ARM_MAGIC) {
            if (f.seq != f.ackSeq && f.seq != g_ackSeq) {
                g_armFile = f;
                DoArm(&f.in);
                g_ackSeq = f.seq;
                Logf("file: arm seq %u consumed", f.seq);
            } else if (f.doRescan || f.doModscan || f.doDisarm) {
                if (f.doDisarm) {
                    ExAcquirePushLockExclusive(&g_lock);
                    if (g_armed)
                        DisarmLocked();
                    ExReleasePushLockExclusive(&g_lock);
                }
                if (f.doRescan)
                    DoRescan();
                if (f.doModscan)
                    DoModscan();
                f.doRescan = 0;
                f.doModscan = 0;
                f.doDisarm = 0;
                HANDLE h2 = NULL;
                NTSTATUS st2 = ZwCreateFile(&h2, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                                            FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                                            FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
                if (NT_SUCCESS(st2)) {
                    IO_STATUS_BLOCK wios;
                    ZwWriteFile(h2, NULL, NULL, NULL, &wios, &f, (ULONG)sizeof(f), NULL, NULL);
                    ZwClose(h2);
                }
            }
        }
    }
    // liveness check: attaching to a dead process froze the whole system once.
    // Do the process-notify job here (mapped images can't register it).
    if (g_armed && g_eproc && g_targetPid) {
        PEPROCESS live = NULL;
        NTSTATUS lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)g_targetPid, &live);
        if (!NT_SUCCESS(lst) || live != g_eproc) {
            if (NT_SUCCESS(lst))
                ObDereferenceObject(live);
            ExAcquirePushLockExclusive(&g_lock);
            Logf("file: target %llu gone - disarming", g_targetPid);
            if (g_armed)
                DisarmLocked();
            ExReleasePushLockExclusive(&g_lock);
            PublishStatus();
            return;
        }
        ObDereferenceObject(live);
    }

    // retry detours from worker context (the notify callback runs under loader
    // locks that can fail page protection changes), and auto-rescan as the
    // IAT-only fallback when the code detour keeps failing
    static ULONG s_tick = 0;
    s_tick++;
    if (g_armed && g_eproc && TargetAlive()) {
        ExAcquirePushLockExclusive(&g_lock);
        BOOLEAN anyPending = FALSE;
        KAPC_STATE apc;
        KeStackAttachProcess((PRKPROCESS)g_eproc, &apc);
        for (ULONG i = 0; i < g_cfg.entryCount; i++) {
            ENTRY_STATE *e = &g_entries[i];
            if (e->exportOk && !e->detoured) {
                anyPending = TRUE;
                if (DetourEntry(e))
                    anyPending = FALSE;      // landed this tick (others may remain)
            }
        }
        KeUnstackDetachProcess(&apc);
        if (anyPending && (s_tick & 3) == 0)     // every ~2s
            DoRescan();
        ExReleasePushLockExclusive(&g_lock);
    }

    PublishStatus();
}

static VOID ControllerDpc(_In_ PKDPC dpc, _In_opt_ PVOID ctx, _In_opt_ PVOID a1, _In_opt_ PVOID a2)
{
    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(a1);
    UNREFERENCED_PARAMETER(a2);
    if (InterlockedCompareExchange(&g_workQueued, 1, 0) == 0)
        ExQueueWorkItem(&g_ctrlWork, DelayedWorkQueue);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNREFERENCED_PARAMETER(drv);
    UNREFERENCED_PARAMETER(reg);

    ExInitializePushLock(&g_lock);
    KeInitializeSpinLock(&g_logLock);
    RtlZeroMemory(g_log, sizeof(g_log));

    // v21: stamp this instance - nonce + load time go into every status file
    {
        g_loadStamp = KeQueryInterruptTime();
        g_instanceNonce = (ULONG)(g_loadStamp >> 8) ^ (ULONG)(ULONG_PTR)&g_instanceNonce ^
                          0x217A1E21u;
        if (g_instanceNonce == 0)
            g_instanceNonce = 1;
        Logf("entry: v21 nonce=%08X loadStamp=%llu", g_instanceNonce, g_loadStamp);
    }

    {
        UNICODE_STRING mmName;
        RtlInitUnicodeString(&mmName, L"MmCopyVirtualMemory");
        g_mmCopy = (RDOC_MM_COPY_FN)MmGetSystemRoutineAddress(&mmName);
    }

    NTSTATUS st = PsSetLoadImageNotifyRoutine(LoadImageNotify);
    Logf("entry: LoadImageNotify register 0x%08X, MmCopy=%u",
         st, g_mmCopy ? 1 : 0);
    // PsSetCreateProcessNotifyRoutine is refused (0xC0000022) from mapped images -
    // the chain handles target-exit cleanup itself


    DoSelftestBoot();

    ExInitializeWorkItem(&g_ctrlWork, ControllerWork, NULL);
    KeInitializeTimer(&g_ctrlTimer);
    KeInitializeDpc(&g_ctrlDpc, ControllerDpc, NULL);

    LARGE_INTEGER due;
    due.QuadPart = -500 * 1000 * 10;      // first fire in 500ms
    ULONG period = 500;                   // then every 500ms
    KeSetTimerEx(&g_ctrlTimer, due, period, &g_ctrlDpc);

    Logf("entry: rdoc_earlyhook file-mode ready (selftest=%u)", g_selfTestResult);
    return STATUS_SUCCESS;
}