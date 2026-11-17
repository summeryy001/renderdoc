// rdcprobe.c - direct target-control client for the injected RenderDoc layer
// on 127.0.0.1:38920. Speaks the streaming chunk protocol:
//   [u32 chunkID][u32 length(=0, ignored)][payload]
// Packets used (target_control.cpp, protocol v9):
//   2 Handshake  c->s: u32 ver=9, str name, u8 force   s->c: u32 ver, str target, u32 pid
//   3 Busy       s->c: u32 ver, str target, str busyclient
//   4 NewCapture s->c: u32 id, u64 ts, str path, buf thumb, u32 driver, u32 frame, u64 size, str title
//   5 APIUse     s->c: u32 driver, u8 presenting, u8 supported, str msg
//   6 TriggerCapture c->s: u32 numFrames
//   11 CaptureProgress s->c: float
//   13 CapturableWindowCount s->c: u32
//   1 Noop / 14 RequestShow: no payload
// build: cl /nologo /O2 rdcprobe.c /Fe:rdcprobe.exe ws2_32.lib
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static SOCKET s_sock = INVALID_SOCKET;

static int SendAll(const void *p, int n)
{
    const char *c = (const char *)p;
    int off = 0;
    while (off < n) {
        int w = send(s_sock, c + off, n - off, 0);
        if (w <= 0) return 0;
        off += w;
    }
    return 1;
}

static int RecvAvail(unsigned char *buf, int cap, int msWait)
{
    // returns bytes read (0 on timeout, -1 on error/close)
    fd_set rf;
    struct timeval tv;
    FD_ZERO(&rf);
    FD_SET(s_sock, &rf);
    tv.tv_sec = msWait / 1000;
    tv.tv_usec = (msWait % 1000) * 1000;
    int r = select(0, &rf, NULL, NULL, &tv);
    if (r <= 0) return 0;
    r = recv(s_sock, (char *)buf, cap, 0);
    return r;
}

typedef struct {
    unsigned char buf[1 << 20];
    int len;
} STREAM;

static int EnsureBytes(STREAM *st, int need, int msTotal)
{
    int waited = 0;
    while (st->len < need && waited < msTotal) {
        int r = RecvAvail(st->buf + st->len, (int)sizeof(st->buf) - st->len, 100);
        if (r < 0) return 0;
        waited += 100;
    }
    return st->len >= need;
}

static unsigned int RdU32(STREAM *st, int *off)
{
    unsigned int v;
    memcpy(&v, st->buf + *off, 4);
    *off += 4;
    return v;
}

static unsigned long long RdU64(STREAM *st, int *off)
{
    unsigned long long v;
    memcpy(&v, st->buf + *off, 8);
    *off += 8;
    return v;
}

// rdcstr: u32 len + bytes (no NUL)
static int RdStr(STREAM *st, int *off, char *out, int cap)
{
    unsigned int n = RdU32(st, off);
    if (n >= (unsigned int)cap) n = (unsigned int)cap - 1;
    memcpy(out, st->buf + *off, n);
    out[n] = 0;
    *off += (int)n;
    return (int)n;
}

static void WrU32(unsigned char *b, int *off, unsigned int v)
{
    memcpy(b + *off, &v, 4);
    *off += 4;
}

static const char *DriverName(unsigned int d)
{
    switch (d) {
    case 0: return "Unknown";
    case 1: return "D3D11";
    case 2: return "OpenGL";
    case 3: return "Mantle";
    case 4: return "D3D12";
    case 5: return "D3D10";
    case 6: return "D3D9";
    case 7: return "Image";
    case 8: return "Vulkan";
    case 9: return "OpenGLES";
    case 10: return "D3D8";
    case 11: return "Metal";
    default: return d >= 100000 ? "Custom" : "?";
    }
}

static void DumpHex(const unsigned char *p, int n)
{
    for (int i = 0; i < n && i < 48; i++)
        printf("%02X ", p[i]);
    printf("\n");
}

static STREAM g_st;

int main(int argc, char **argv)
{
    int listenMs = argc > 1 ? atoi(argv[1]) : 3000;   // phase 1: listen
    int afterMs = argc > 2 ? atoi(argv[2]) : 9000;    // phase 2: post-trigger
    const char *host = argc > 3 ? argv[3] : "127.0.0.1";
    int port = argc > 4 ? atoi(argv[4]) : 38920;
    int requireApi = argc > 5 && strcmp(argv[5], "require-api") == 0;

    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host, &sa.sin_addr);

    s_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s_sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("connect failed %d\n", WSAGetLastError());
        return 1;
    }
    printf("connected to %s:%d\n", host, port);

    // ---- send client handshake: chunk 2, payload {u32 9, str "rdcprobe", u8 0}
    unsigned char pkt[128];
    int o = 0;
    WrU32(pkt, &o, 2);          // ePacket_Handshake
    WrU32(pkt, &o, 0);          // length (streaming: 0)
    WrU32(pkt, &o, 9);          // protocol version
    WrU32(pkt, &o, 8);          // clientName length
    memcpy(pkt + o, "rdcprobe", 8);
    o += 8;
    pkt[o++] = 0;               // forceConnection = false
    if (!SendAll(pkt, o)) { printf("send handshake failed\n"); return 1; }
    printf("handshake sent (%d bytes)\n", o);

    static STREAM st;   // 1MB buffer: static, not on the stack
    st.len = 0;
    int triggered = 0;
    int apiRegistered = 0;
    int phaseEnd = GetTickCount() + listenMs;
    int totalEnd = phaseEnd + afterMs;
    char str1[1024], str2[1024], str3[1024];

    while (GetTickCount() < totalEnd) {
        if (!EnsureBytes(&st, 8, 200)) {
            if (st.len == 0) continue;
        }
        if (st.len < 8) continue;
        int off = 0;
        unsigned int id = RdU32(&st, &off);
        unsigned int len = RdU32(&st, &off);
        (void)len;

        // figure payload needs per packet
        int need;
        switch (id) {
        case 1: need = 0; break;                 // Noop
        case 14: need = 0; break;                // RequestShow
        case 11: need = 4; break;                // CaptureProgress (float)
        case 13: need = 4; break;                // CapturableWindowCount
        default: need = 16; break;               // variable: get at least a start
        }
        if (!EnsureBytes(&st, 8 + need, 1500)) {
            printf("[timeout waiting payload for chunk %u]\n", id);
            break;
        }

        switch (id) {
        case 1:
            printf("Noop\n");
            break;
        case 2: {
            unsigned int ver = RdU32(&st, &off);
            RdStr(&st, &off, str1, sizeof(str1));
            unsigned int pid = RdU32(&st, &off);
            printf("HANDSHAKE: target='%s' pid=%u version=%u\n", str1, pid, ver);
            break;
        }
        case 3: {
            unsigned int ver = RdU32(&st, &off);
            RdStr(&st, &off, str1, sizeof(str1));
            RdStr(&st, &off, str2, sizeof(str2));
            printf("BUSY: target='%s' owned by '%s' (ver %u)\n>>> close the other client "
                   "(qrendertest?) and rerun\n", str1, str2, ver);
            return 2;
        }
        case 4: {
            unsigned int capid = RdU32(&st, &off);
            unsigned long long ts = RdU64(&st, &off);
            RdStr(&st, &off, str1, sizeof(str1));       // path
            unsigned int thumbLen = RdU32(&st, &off);
            // ensure thumb bytes present
            if (!EnsureBytes(&st, off + (int)thumbLen + 20, 2000)) {
                printf("NewCapture: path='%s' (thumb %u bytes, truncated follow-up)\n",
                       str1, thumbLen);
                goto consumed_partial;
            }
            off += (int)thumbLen;
            unsigned int driver = RdU32(&st, &off);
            unsigned int frame = RdU32(&st, &off);
            unsigned long long size = RdU64(&st, &off);
            RdStr(&st, &off, str2, sizeof(str2));       // title (v9)
            printf("*** NEW CAPTURE: id=%u frame=%u driver=%s size=%llu\n    path='%s'\n"
                   "    title='%s' ts=%llu\n",
                   capid, frame, DriverName(driver), size, str1, str2, ts);
            break;
        }
        case 5: {
            unsigned int driver = RdU32(&st, &off);
            unsigned char presenting = st.buf[off++];
            unsigned char supported = st.buf[off++];
            RdStr(&st, &off, str1, sizeof(str1));
            printf("APIUse: %s  presenting=%s supported=%s%s%s\n", DriverName(driver),
                   presenting ? "YES" : "no", supported ? "YES" : "no",
                   str1[0] ? " msg='" : "", str1[0] ? str1 : "");
            if ((driver == 1 || driver == 4) && supported) {
                apiRegistered = 1;
                printf("RDOC_DIAG API_REGISTERED driver=%s presenting=%s\n",
                       DriverName(driver), presenting ? "YES" : "no");
            }
            break;
        }
        case 11: {
            float f;
            memcpy(&f, st.buf + off, 4);
            off += 4;
            if (f >= 0.0f) printf("CaptureProgress %.0f%%\n", f * 100.0f);
            break;
        }
        case 13: {
            unsigned int w = RdU32(&st, &off);
            printf("CapturableWindowCount=%u\n", w);
            break;
        }
        case 14:
            printf("RequestShow\n");
            break;
        default:
            printf("chunk %u (len %u) raw: ", id, len);
            DumpHex(st.buf + 8, st.len - 8 < 48 ? st.len - 8 : 48);
            off = st.len;    // unknown: consume all to resync at next boundary risk
            break;
        }

    consumed_partial:
        // shift remaining bytes
        if (off < st.len) {
            memmove(st.buf, st.buf + off, st.len - off);
            st.len -= off;
        } else {
            st.len = 0;
        }

        if (!triggered && GetTickCount() >= phaseEnd && (!requireApi || apiRegistered)) {
            int t = 0;
            WrU32(pkt, &t, 6);   // ePacket_TriggerCapture
            WrU32(pkt, &t, 0);
            WrU32(pkt, &t, 1);   // numFrames = 1
            if (SendAll(pkt, t)) printf(">>> TRIGGER CAPTURE sent\n");
            triggered = 1;
        }
    }

    closesocket(s_sock);
    WSACleanup();
    if (requireApi && !apiRegistered) {
        printf("RDOC_DIAG API_NOT_REGISTERED trigger_suppressed=1\n");
        return 3;
    }
    printf("done\n");
    return 0;
}
