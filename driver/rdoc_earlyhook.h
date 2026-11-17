// rdoc_earlyhook.h - shared protocol between rdoc_earlyhook.sys and user-mode controller
// Phase-2 driver: kernel load-time takeover for RenderDoc against header-erasing anti-cheat.
//
// Contract:
//   1. ARM (from the injection chain, during the pre-init freeze window):
//        - targetPid  : game process (Client-Win64-Shipping)
//        - entries[]  : wrapper abs addr + orig-store abs addr (rendertest copy base + PDB RVA)
//                       + watched dll name (dxgi.dll / d3d12.dll) + export name
//   2. Driver waits for the watched dll to be mapped into targetPid
//      (PsSetLoadImageNotifyRoutine - fires before any user code incl. anti-cheat erasure,
//       and BEFORE the loader snaps importers' IATs, so no loader-overwrite problem).
//   3. On map, in the loader thread (= target process context):
//        a. parse the mapped image's export table  -> realAddr (entry in the loaded dll)
//        b. map a CLEAN private copy of the same dll into the target (SEC_IMAGE, no loader list)
//           -> origAddr (entry in the clean copy)
//        c. write origAddr into the wrapper's orig_funcptr store  (MANDATORY before detour:
//           dxgi wrappers call orig unconditionally; d3d12 wrapper falls back to
//           GetProcAddress which would hit our detour -> infinite recursion)
//        d. detour realAddr: mov rax, wrapperAbs; jmp rax  (12 bytes, COW-safe via
//           ZwProtectVirtualMemory, per-process only)
//   4. Every later resolution path (static IAT snap, GetProcAddress, internal) lands in the
//      wrapper; the wrapper calls the clean-copy original. No recursion, no dependence on
//      PE headers surviving in the target (we never read them again).
//   5. RESCAN (repair pass): attach to target, scan supplied ranges for QWORD == realAddr,
//      exchange to wrapperAddr (covers IATs snapped before the detour existed / if the
//      anti-cheat reverts code bytes; data cells survive code CRC patrols).
//
// Build: WDK 10.0.26100, VS2022, x64, WDM, test-signed.

#pragma once

#ifndef RDOC_EARLYHOOK_H
#define RDOC_EARLYHOOK_H

#define RDOC_MAX_ENTRIES 8
#define RDOC_MAX_RANGES  64
#define RDOC_LOG_SIZE    16384

// ---- IOCTL codes ----
#define RDOC_IOCTL_ARM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RDOC_IOCTL_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RDOC_IOCTL_RESCAN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RDOC_IOCTL_DISARM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RDOC_IOCTL_LOG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---- ARM ----
typedef struct _RDOC_ARM_ENTRY {
    unsigned __int64 wrapperAbs;    // abs addr of wrapper function inside target process
    unsigned __int64 origStoreAbs;  // abs addr of HookedFunction::orig_funcptr store in target
    wchar_t dllName[32];            // watched image basename, e.g. L"dxgi.dll"
    char   funcName[64];            // export name, e.g. "CreateDXGIFactory1"
    wchar_t srcPath[160];           // optional Win32/native source path for the clean copy
                                    // (e.g. Agility D3D12Core.dll inside the game dir);
                                    // empty = \SystemRoot\System32\<dllName>
} RDOC_ARM_ENTRY, *PRDOC_ARM_ENTRY;

typedef struct _RDOC_ARM_IN {
    unsigned __int64 targetPid;     // game process id
    unsigned __int32 entryCount;    // <= RDOC_MAX_ENTRIES
    unsigned __int32 flags;         // bit0: detour enabled, bit1: orig-fill enabled
    RDOC_ARM_ENTRY entries[RDOC_MAX_ENTRIES];
} RDOC_ARM_IN, *PRDOC_ARM_IN;

#define RDOC_FLAG_DETOUR    0x1
#define RDOC_FLAG_ORIGFILL  0x2

// ---- STATUS ----
typedef struct _RDOC_STATUS_ENTRY {
    unsigned __int64 realAddr;      // export in the loaded watched image (0 = not seen yet)
    unsigned __int64 origAddr;      // export in the clean copy
    unsigned __int32 state;         // RDOC_ES_*
    unsigned __int32 reserved;
    __int64          scanHits;      // QWORD cells rewritten by RESCAN
} RDOC_STATUS_ENTRY, *PRDOC_STATUS_ENTRY;

#define RDOC_ES_DLLSEEN   0x01
#define RDOC_ES_EXPORT_OK 0x02
#define RDOC_ES_CLEAN_OK  0x04
#define RDOC_ES_ORIGFILL  0x08
#define RDOC_ES_DETOURED  0x10
#define RDOC_ES_REVERTED  0x20      // bytes no longer match our detour (AC patrol touched it)
#define RDOC_ES_IATOK     0x40      // clean copy's IAT copied from the loader-snapped image

typedef struct _RDOC_STATUS_OUT {
    unsigned __int64 targetPid;
    unsigned __int32 armed;
    unsigned __int32 entryCount;
    RDOC_STATUS_ENTRY entries[RDOC_MAX_ENTRIES];
} RDOC_STATUS_OUT, *PRDOC_STATUS_OUT;

// ---- RESCAN ----
typedef struct _RDOC_RANGE {
    unsigned __int64 base;
    unsigned __int64 size;
} RDOC_RANGE, *PRDOC_RANGE;

typedef struct _RDOC_RESCAN_IN {
    unsigned __int32 rangeCount;    // <= RDOC_MAX_RANGES
    unsigned __int32 reserved;
    RDOC_RANGE ranges[RDOC_MAX_RANGES];
} RDOC_RESCAN_IN, *PRDOC_RESCAN_IN;

typedef struct _RDOC_RESCAN_OUT {
    unsigned __int64 bytesScanned;
    __int64          totalHits;
    __int64          hitsPerEntry[RDOC_MAX_ENTRIES];
} RDOC_RESCAN_OUT, *PRDOC_RESCAN_OUT;

// ---- LOG ----
typedef struct _RDOC_LOG_OUT {
    unsigned __int32 total;         // total bytes the ring currently holds
    unsigned __int32 reserved;
    // followed by 'total' bytes of log text (NUL-terminated)
} RDOC_LOG_OUT, *PRDOC_LOG_OUT;

// ---- SELFTEST (kernel-path validation inside the CALLER's process, no game needed) ----
#define RDOC_IOCTL_SELFTEST \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RDOC_ST_DETOUR 0x1          // also apply + revert a detour on the resolved entry

typedef struct _RDOC_SELFTEST_IN {
    wchar_t dllName[32];
    char   funcName[64];
    wchar_t srcPath[160];           // optional; empty = System32
    unsigned __int32 flags;
    unsigned __int32 reserved;
} RDOC_SELFTEST_IN, *PRDOC_SELFTEST_IN;

typedef struct _RDOC_SELFTEST_OUT {
    unsigned __int64 cleanBase;
    unsigned __int64 resolvedAddr;  // export entry in the clean copy
    unsigned __int32 mapOk;
    unsigned __int32 exportOk;
    unsigned __int32 ldeOk;         // prologue reached >=12 whole bytes
    unsigned __int32 trampLen;      // trampoline length (0 if LDE refused)
    unsigned __int32 detourApplied;
    unsigned __int32 detourRestored;
    unsigned __int32 status;        // 0 = all pass
} RDOC_SELFTEST_OUT, *PRDOC_SELFTEST_OUT;

#endif // RDOC_EARLYHOOK_H
