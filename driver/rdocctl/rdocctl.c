// rdocctl.c - user-mode controller for rdoc_earlyhook.sys
// build: cl /nologo /W4 /O2 rdocctl.c /Fe:rdocctl.exe
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initguid.h>
#include <winioctl.h>
#include "../rdoc_earlyhook.h"

#define DEV L"\\\\.\\RdocEarlyHook"

static HANDLE Open(void)
{
    HANDLE h = CreateFileW(DEV, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        fprintf(stderr, "open %ls failed %lu (driver loaded?)\n", DEV, GetLastError());
    return h;
}

static void PrintStatus(PRDOC_STATUS_OUT st)
{
    printf("armed=%u pid=%llu entries=%u\n", st->armed, st->targetPid, st->entryCount);
    for (DWORD i = 0; i < st->entryCount && i < RDOC_MAX_ENTRIES; i++) {
        printf("  [%u] real=0x%llx orig=0x%llx hits=%lld state=0x%x %s%s%s%s%s%s\n",
               i, st->entries[i].realAddr, st->entries[i].origAddr,
               st->entries[i].scanHits, st->entries[i].state,
               (st->entries[i].state & RDOC_ES_DLLSEEN)   ? "DLLSEEN "  : "",
               (st->entries[i].state & RDOC_ES_EXPORT_OK) ? "EXPORT "   : "",
               (st->entries[i].state & RDOC_ES_CLEAN_OK)  ? "CLEAN "    : "",
               (st->entries[i].state & RDOC_ES_ORIGFILL)  ? "ORIG "     : "",
               (st->entries[i].state & RDOC_ES_DETOURED)  ? "DETOUR "   : "",
               (st->entries[i].state & RDOC_ES_REVERTED)  ? "REVERTED " : "");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf(
            "usage:\n"
            "  rdocctl arm --pid N [--nodetour] [--noorig] [--entry wrapper=0x.. orig=0x.. dll=dxgi.dll func=CreateDXGIFactory1]..\n"
            "  rdocctl status\n"
            "  rdocctl rescan [--range 0xbase,0xsize]..   (no --range = kernel auto walk of image RW regions)\n"
            "  rdocctl disarm\n"
            "  rdocctl log\n"
            "file-mode (kdmapper):\n"
            "  rdocctl armfile | readstatus | rescanfile | modscanfile | disarmfile\n");
        return 1;
    }

    if (!strcmp(argv[1], "parse-test")) {
        // offline validation of the ARM argument pipeline: feed the exact strings
        // chain_kernel.ps1 generates, see what the driver would receive.
        RDOC_ARM_IN in;
        ZeroMemory(&in, sizeof(in));
        in.flags = RDOC_FLAG_DETOUR | RDOC_FLAG_ORIGFILL;
        DWORD n = 0;
        for (int i = 2; i < argc; i++) {
            if (!strncmp(argv[i], "--pid=", 6))
                in.targetPid = _strtoui64(argv[i] + 6, NULL, 0);
            else if (!strncmp(argv[i], "--entry=", 8)) {
                if (n >= RDOC_MAX_ENTRIES) { printf("too many entries\n"); return 1; }
                RDOC_ARM_ENTRY *e = &in.entries[n];
                char buf[1024];
                strncpy(buf, argv[i] + 8, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                char *ctx = NULL, *tok = strtok_s(buf, " ", &ctx);
                while (tok) {
                    if (!strncmp(tok, "wrapper=", 8)) e->wrapperAbs = _strtoui64(tok + 8, NULL, 0);
                    else if (!strncmp(tok, "orig=", 5)) e->origStoreAbs = _strtoui64(tok + 5, NULL, 0);
                    else if (!strncmp(tok, "dll=", 4)) {
                        WCHAR w[32] = {0};
                        MultiByteToWideChar(CP_UTF8, 0, tok + 4, -1, w, 31);
                        wcsncpy(e->dllName, w, 31);
                        e->dllName[31] = 0;
                    }
                    else if (!strncmp(tok, "func=", 5)) {
                        strncpy(e->funcName, tok + 5, sizeof(e->funcName) - 1);
                        e->funcName[sizeof(e->funcName) - 1] = 0;
                    }
                    tok = strtok_s(NULL, " ", &ctx);
                }
                n++;
            }
        }
        in.entryCount = n;
        printf("pid=%llu count=%u flags=0x%x sizeof(entry)=%zu sizeof(in)=%zu\n",
               in.targetPid, in.entryCount, in.flags, sizeof(RDOC_ARM_ENTRY), sizeof(RDOC_ARM_IN));
        int fails = 0;
        for (DWORD i = 0; i < n; i++) {
            char dllA[64] = {0};
            WideCharToMultiByte(CP_UTF8, 0, in.entries[i].dllName, -1, dllA, 63, NULL, NULL);
            int ok = in.entries[i].wrapperAbs > 0x10000 && in.entries[i].origStoreAbs > 0x10000 &&
                     in.entries[i].dllName[0] && in.entries[i].funcName[0];
            if (!ok) fails++;
            printf("  [%u] wrapper=0x%llX orig=0x%llX dll=%ls func=%s %s\n",
                   i, in.entries[i].wrapperAbs, in.entries[i].origStoreAbs,
                   in.entries[i].dllName, in.entries[i].funcName, ok ? "OK" : "BAD");
        }
        printf(fails ? "PARSE-TEST FAIL(%d)\n" : "PARSE-TEST PASS\n", fails);
        return fails ? 2 : 0;
    }

    if (!strcmp(argv[1], "armfile") || !strcmp(argv[1], "readstatus") ||
        !strcmp(argv[1], "rescanfile") || !strcmp(argv[1], "modscanfile") ||
        !strcmp(argv[1], "disarmfile")) {
        // file-mode control plane (driver mapped by kdmapper, no device)
        const char *armPath = "C:\\rdoc_arm.bin";
        const char *stPath  = "C:\\rdoc_status.bin";
        #pragma pack(push,1)
        typedef struct { ULONG magic; ULONG seq; ULONG ackSeq; ULONG doRescan;
                         ULONG doModscan; ULONG doDisarm; RDOC_ARM_IN in; } ARM_FILE;
        #pragma pack(pop)
        if (!strcmp(argv[1], "rescanfile") || !strcmp(argv[1], "modscanfile") ||
            !strcmp(argv[1], "disarmfile")) {
            ARM_FILE f;
            ZeroMemory(&f, sizeof(f));
            HANDLE hf = CreateFileA(armPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (hf == INVALID_HANDLE_VALUE) { printf("no arm file\n"); return 1; }
            DWORD rd = 0; ReadFile(hf, &f, sizeof(f), &rd, NULL); CloseHandle(hf);
            if (f.magic != 0x31314452u) { printf("bad magic\n"); return 1; }
            if (!strcmp(argv[1], "rescanfile"))  f.doRescan = 1;
            if (!strcmp(argv[1], "modscanfile")) f.doModscan = 1;
            if (!strcmp(argv[1], "disarmfile"))  f.doDisarm = 1;
            hf = CreateFileA(armPath, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
            if (hf == INVALID_HANDLE_VALUE) { printf("trunc failed %lu\n", GetLastError()); return 1; }
            WriteFile(hf, &f, sizeof(f), &rd, NULL);
            CloseHandle(hf);
            printf("%s requested\n", argv[1]);
            return 0;
        }
        if (!strcmp(argv[1], "armfile")) {
            RDOC_ARM_IN in;
            ZeroMemory(&in, sizeof(in));
            in.flags = RDOC_FLAG_DETOUR | RDOC_FLAG_ORIGFILL;
            DWORD n = 0;
            for (int i = 2; i < argc; i++) {
                if (!strncmp(argv[i], "--pid=", 6))
                    in.targetPid = _strtoui64(argv[i] + 6, NULL, 0);
                else if (!strncmp(argv[i], "--entry=", 8)) {
                    if (n >= RDOC_MAX_ENTRIES) { printf("too many entries\n"); return 1; }
                    RDOC_ARM_ENTRY *e = &in.entries[n];
                    char buf[1024];
                    strncpy(buf, argv[i] + 8, sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = 0;
                    char *ctx = NULL, *tok = strtok_s(buf, " ", &ctx);
                    while (tok) {
                        if (!strncmp(tok, "wrapper=", 8)) e->wrapperAbs = _strtoui64(tok + 8, NULL, 0);
                        else if (!strncmp(tok, "orig=", 5)) e->origStoreAbs = _strtoui64(tok + 5, NULL, 0);
                        else if (!strncmp(tok, "dll=", 4)) {
                            WCHAR w[32] = {0};
                            MultiByteToWideChar(CP_UTF8, 0, tok + 4, -1, w, 31);
                            wcsncpy(e->dllName, w, 31);
                        }
                        else if (!strncmp(tok, "func=", 5)) {
                            strncpy(e->funcName, tok + 5, sizeof(e->funcName) - 1);
                        }
                        tok = strtok_s(NULL, " ", &ctx);
                    }
                    n++;
                }
            }
            in.entryCount = n;
            // read-modify-write the arm file with a fresh sequence
            #pragma pack(push,1)
            typedef struct { ULONG magic; ULONG seq; ULONG ackSeq; ULONG doRescan;
                             ULONG doModscan; ULONG doDisarm; RDOC_ARM_IN in; } ARM_FILE;
            #pragma pack(pop)
            ARM_FILE f;
            ZeroMemory(&f, sizeof(f));
            HANDLE hf = CreateFileA(armPath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD rd = 0;
                ReadFile(hf, &f, sizeof(f), &rd, NULL);
                CloseHandle(hf);
            }
            f.magic = 0x31314452u;      // 'RDA1'
            f.seq = (f.ackSeq >= f.seq ? f.ackSeq : f.seq) + 1;
            f.ackSeq = 0;
            f.doRescan = 0;
            f.doModscan = 0;
            f.doDisarm = 0;
            f.in = in;
            hf = CreateFileA(armPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (hf == INVALID_HANDLE_VALUE) { printf("write %s failed %lu\n", armPath, GetLastError()); return 1; }
            DWORD wr = 0;
            WriteFile(hf, &f, sizeof(f), &wr, NULL);
            CloseHandle(hf);
            printf("ARMFILE ok (seq %u, %u entries)\n", f.seq, n);
            return 0;
        }
        // readstatus
        #pragma pack(push,1)
        typedef struct { ULONG magic; ULONG ackSeq; ULONG armed; ULONG entryCount; ULONG selfTest;
                         ULONG _pad;              /* natural 8-alignment of following UINT64s */
                         RDOC_STATUS_ENTRY entries[8];
                         char logTail[512]; } ST_FILE;
        #pragma pack(pop)
        ST_FILE st;
        ZeroMemory(&st, sizeof(st));
        HANDLE hf = CreateFileA(stPath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (hf == INVALID_HANDLE_VALUE) { printf("no status file (driver running?)\n"); return 1; }
        DWORD rd = 0;
        ReadFile(hf, &st, sizeof(st), &rd, NULL);
        CloseHandle(hf);
        if (rd < sizeof(st) || st.magic != 0x31534452u) { printf("bad status file\n"); return 1; }
        printf("armed=%u entries=%u selftest=%u ack=%u\n", st.armed, st.entryCount, st.selfTest, st.ackSeq);
        for (DWORD i = 0; i < st.entryCount && i < 8; i++)
            printf("  [%u] real=0x%llx orig=0x%llx hits=%lld state=0x%x %s%s%s%s%s%s\n",
                   i, st.entries[i].realAddr, st.entries[i].origAddr,
                   st.entries[i].scanHits, st.entries[i].state,
                   (st.entries[i].state & 0x01) ? "DLLSEEN " : "",
                   (st.entries[i].state & 0x02) ? "EXPORT " : "",
                   (st.entries[i].state & 0x04) ? "CLEAN " : "",
                   (st.entries[i].state & 0x08) ? "ORIG " : "",
                   (st.entries[i].state & 0x10) ? "DETOUR " : "",
                   (st.entries[i].state & 0x20) ? "REVERTED " : "");
        st.logTail[sizeof(st.logTail) - 1] = 0;
        printf("--- driver log tail ---\n%s\n", st.logTail);
        return 0;
    }
    HANDLE h = Open();
    if (h == INVALID_HANDLE_VALUE)
        return 1;

    if (!strcmp(argv[1], "arm")) {
        RDOC_ARM_IN in;
        ZeroMemory(&in, sizeof(in));
        in.flags = RDOC_FLAG_DETOUR | RDOC_FLAG_ORIGFILL;
        DWORD n = 0;
        for (int i = 2; i < argc; i++) {
            if (!strncmp(argv[i], "--pid=", 6))
                in.targetPid = _strtoui64(argv[i] + 6, NULL, 0);
            else if (!strcmp(argv[i], "--nodetour"))
                in.flags &= ~RDOC_FLAG_DETOUR;
            else if (!strcmp(argv[i], "--noorig"))
                in.flags &= ~RDOC_FLAG_ORIGFILL;
            else if (!strncmp(argv[i], "--entry=", 8)) {
                if (n >= RDOC_MAX_ENTRIES) { printf("too many entries\n"); return 1; }
                RDOC_ARM_ENTRY *e = &in.entries[n];
                char buf[1024];
                strncpy(buf, argv[i] + 8, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                char *ctx = NULL, *tok = strtok_s(buf, " ", &ctx);
                while (tok) {
                    if (!strncmp(tok, "wrapper=", 8)) e->wrapperAbs = _strtoui64(tok + 8, NULL, 0);
                    else if (!strncmp(tok, "orig=", 5)) e->origStoreAbs = _strtoui64(tok + 5, NULL, 0);
                    else if (!strncmp(tok, "dll=", 4)) {
                        WCHAR w[32] = {0};
                        MultiByteToWideChar(CP_UTF8, 0, tok + 4, -1, w, 31);
                        wcsncpy(e->dllName, w, 31);
                        e->dllName[31] = 0;
                    }
                    else if (!strncmp(tok, "func=", 5)) {
                        strncpy(e->funcName, tok + 5, sizeof(e->funcName) - 1);
                        e->funcName[sizeof(e->funcName) - 1] = 0;
                    }
                    tok = strtok_s(NULL, " ", &ctx);
                }
                n++;
            }
        }
        in.entryCount = n;
        DWORD br = 0;
        if (!DeviceIoControl(h, RDOC_IOCTL_ARM, &in, sizeof(in), NULL, 0, &br, NULL)) {
            printf("ARM failed %lu\n", GetLastError());
            return 1;
        }
        printf("ARM ok (%u entries, flags 0x%x)\n", n, in.flags);
    }
    else if (!strcmp(argv[1], "status")) {
        RDOC_STATUS_OUT st;
        DWORD br = 0;
        if (!DeviceIoControl(h, RDOC_IOCTL_STATUS, NULL, 0, &st, sizeof(st), &br, NULL)) {
            printf("STATUS failed %lu\n", GetLastError());
            return 1;
        }
        PrintStatus(&st);
    }
    else if (!strcmp(argv[1], "rescan")) {
        RDOC_RESCAN_IN in;
        RDOC_RESCAN_OUT out;
        ZeroMemory(&in, sizeof(in));
        DWORD n = 0;
        for (int i = 2; i < argc; i++) {
            if (!strncmp(argv[i], "--range=", 8) && n < RDOC_MAX_RANGES) {
                char buf[256];
                strncpy(buf, argv[i] + 8, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                char *comma = strchr(buf, ',');
                if (comma) {
                    *comma = 0;
                    in.ranges[n].base = _strtoui64(buf, NULL, 0);
                    in.ranges[n].size = _strtoui64(comma + 1, NULL, 0);
                    n++;
                }
            }
        }
        in.rangeCount = n;
        DWORD br = 0;
        if (!DeviceIoControl(h, RDOC_IOCTL_RESCAN, &in, sizeof(in), &out, sizeof(out), &br, NULL)) {
            printf("RESCAN failed %lu\n", GetLastError());
            return 1;
        }
        printf("RESCAN: scanned %llu bytes, total hits %lld\n", out.bytesScanned, out.totalHits);
        for (DWORD i = 0; i < RDOC_MAX_ENTRIES; i++)
            if (out.hitsPerEntry[i])
                printf("  entry[%u] hits %lld\n", i, out.hitsPerEntry[i]);
    }
    else if (!strcmp(argv[1], "disarm")) {
        DWORD br = 0;
        if (!DeviceIoControl(h, RDOC_IOCTL_DISARM, NULL, 0, NULL, 0, &br, NULL)) {
            printf("DISARM failed %lu\n", GetLastError());
            return 1;
        }
        printf("DISARM ok\n");
    }
    else if (!strcmp(argv[1], "log")) {
        BYTE buf[sizeof(RDOC_LOG_OUT) + RDOC_LOG_SIZE + 16];
        DWORD br = 0;
        if (!DeviceIoControl(h, RDOC_IOCTL_LOG, NULL, 0, buf, sizeof(buf), &br, NULL)) {
            printf("LOG failed %lu\n", GetLastError());
            return 1;
        }
        PRDOC_LOG_OUT lo = (PRDOC_LOG_OUT)buf;
        printf("%.*s", lo->total, (char *)(buf + sizeof(RDOC_LOG_OUT)));
    }
    else if (!strcmp(argv[1], "selftest")) {
        RDOC_SELFTEST_IN in;
        RDOC_SELFTEST_OUT out;
        ZeroMemory(&in, sizeof(in));
        ZeroMemory(&out, sizeof(out));
        strncpy(in.funcName, "CreateDXGIFactory1", sizeof(in.funcName) - 1);
        wcscpy_s(in.dllName, 32, L"dxgi.dll");
        DWORD br = 0;
        for (int i = 2; i < argc; i++) {
            if (!strncmp(argv[i], "--dll=", 6)) {
                WCHAR w[32] = {0};
                MultiByteToWideChar(CP_UTF8, 0, argv[i] + 6, -1, w, 31);
                wcsncpy(in.dllName, w, 31);
            }
            else if (!strncmp(argv[i], "--func=", 7))
                strncpy(in.funcName, argv[i] + 7, sizeof(in.funcName) - 1);
            else if (!strncmp(argv[i], "--src=", 6)) {
                WCHAR w[160] = {0};
                MultiByteToWideChar(CP_UTF8, 0, argv[i] + 6, -1, w, 159);
                wcsncpy(in.srcPath, w, 159);
            }
            else if (!strcmp(argv[i], "--detour"))
                in.flags |= RDOC_ST_DETOUR;
        }
        if (!DeviceIoControl(h, RDOC_IOCTL_SELFTEST, &in, sizeof(in), &out, sizeof(out), &br, NULL)) {
            printf("SELFTEST failed %lu\n", GetLastError());
            return 1;
        }
        printf("SELFTEST: status=%u map=%u export=%u lde=%u trampLen=%u cleanBase=0x%llx resolved=0x%llx "
               "detourApplied=%u detourRestored=%u\n",
               out.status, out.mapOk, out.exportOk, out.ldeOk, out.trampLen,
               (unsigned long long)out.cleanBase, (unsigned long long)out.resolvedAddr,
               out.detourApplied, out.detourRestored);
        return out.status == 0 ? 0 : 2;
    }
    else {
        printf("unknown command %s\n", argv[1]);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
