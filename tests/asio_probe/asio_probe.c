/*
 * Minimal ASIO host used by tests/asio_probe/run*.sh.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ASIO interface mirror. */

typedef struct w_int64_t
{
    ULONG hi;
    ULONG lo;
} w_int64_t;

typedef struct BufferInformation
{
    LONG  isInputType;
    LONG  channelNumber;
    void *audioBufferStart;
    void *audioBufferEnd;
} BufferInformation;

typedef struct TimeInformation
{
    LONG      _1[4];
    double    _2;
    w_int64_t timeStamp;
    w_int64_t numSamples;
    double    sampleRate;
    ULONG     flags;
    char      _3[12];
    double    speedForTimeCode;
    w_int64_t timeStampForTimeCode;
    ULONG     flagsForTimeCode;
    char      _4[64];
} TimeInformation;

/* i386 ASIO methods are thiscall; host callbacks are cdecl. */
#ifdef __i386__
#define PROBE_THISCALL __attribute__((__thiscall__))
#define PROBE_CB /* cdecl */
#else
#define PROBE_THISCALL CALLBACK
#define PROBE_CB CALLBACK
#endif

typedef struct Callbacks
{
    void(PROBE_CB *swapBuffers)(LONG, LONG);
    void(PROBE_CB *sampleRateChanged)(double);
    LONG(PROBE_CB *sendNotification)(LONG, LONG, void *, double *);
    void *(PROBE_CB *swapBuffersWithTimeInfo)(TimeInformation *, LONG, LONG);
} Callbacks;

/* IPipeASIO COM vtable mirror. */
typedef struct IPipeASIO IPipeASIO;
typedef struct IPipeASIOVtbl
{
    HRESULT(CALLBACK *QueryInterface)(IPipeASIO *, REFIID, void **);
    ULONG(CALLBACK *AddRef)(IPipeASIO *);
    ULONG(CALLBACK *Release)(IPipeASIO *);
    LONG(PROBE_THISCALL *Init)(IPipeASIO *, void *);
    void(PROBE_THISCALL *GetDriverName)(IPipeASIO *, char *);
    LONG(PROBE_THISCALL *GetDriverVersion)(IPipeASIO *);
    void(PROBE_THISCALL *GetErrorMessage)(IPipeASIO *, char *);
    LONG(PROBE_THISCALL *Start)(IPipeASIO *);
    LONG(PROBE_THISCALL *Stop)(IPipeASIO *);
    LONG(PROBE_THISCALL *GetChannels)(IPipeASIO *, LONG *, LONG *);
    LONG(PROBE_THISCALL *GetLatencies)(IPipeASIO *, LONG *, LONG *);
    LONG(PROBE_THISCALL *GetBufferSize)(IPipeASIO *, LONG *, LONG *, LONG *, LONG *);
    LONG(PROBE_THISCALL *CanSampleRate)(IPipeASIO *, double);
    LONG(PROBE_THISCALL *GetSampleRate)(IPipeASIO *, double *);
    LONG(PROBE_THISCALL *SetSampleRate)(IPipeASIO *, double);
    LONG(PROBE_THISCALL *GetClockSources)(IPipeASIO *, void *, LONG *);
    LONG(PROBE_THISCALL *SetClockSource)(IPipeASIO *, LONG);
    LONG(PROBE_THISCALL *GetSamplePosition)(IPipeASIO *, w_int64_t *, w_int64_t *);
    LONG(PROBE_THISCALL *GetChannelInfo)(IPipeASIO *, void *);
    LONG(PROBE_THISCALL *CreateBuffers)(IPipeASIO *, BufferInformation *, LONG, LONG, Callbacks *);
    LONG(PROBE_THISCALL *DisposeBuffers)(IPipeASIO *);
    LONG(PROBE_THISCALL *ControlPanel)(IPipeASIO *);
    LONG(PROBE_THISCALL *Future)(IPipeASIO *, LONG, void *);
    LONG(PROBE_THISCALL *OutputReady)(IPipeASIO *);
} IPipeASIOVtbl;
struct IPipeASIO
{
    const IPipeASIOVtbl *lpVtbl;
};

/* Must match src/asio.c and src/regsvr.c. */
static const GUID CLSID_PipeASIO
        = { 0x2D3CA9E2, 0x1193, 0x4C5D, { 0xB5, 0xFD, 0x38, 0x79, 0x8F, 0x3D, 0xC0, 0x74 } };

/* Probe state. */

static volatile LONG g_cycles;
static volatile LONG g_stop;

static void PROBE_CB
cb_swapBuffers(LONG idx, LONG direct)
{
    (void)idx;
    (void)direct;
    g_cycles++;
}
static void PROBE_CB
cb_sampleRateChanged(double rate)
{
    fprintf(stderr, "[probe] sampleRateChanged(%f)\n", rate);
}
static LONG PROBE_CB
cb_sendNotification(LONG selector, LONG value, void *msg, double *opt)
{
    (void)value;
    (void)msg;
    (void)opt;
    /* Advertise the callback paths this probe implements. */
    if (selector == 1 || selector == 2)
        return 1;
    if (selector == 7 /* kAsioSupportsTimeInfo */)
        return 1;
    return 0;
}
static void *PROBE_CB
cb_swapBuffersWithTimeInfo(TimeInformation *t, LONG idx, LONG direct)
{
    (void)t;
    (void)idx;
    (void)direct;
    g_cycles++;
    return NULL;
}

/* Main probe sequence. */

static int
die(const char *what, LONG err)
{
    fprintf(stderr, "[probe] FAIL: %s -> 0x%lx\n", what, (unsigned long)err);
    return 1;
}

/* winecrt0 hands main() argc=0 under -mno-cygwin on current Wine, so
 * rebuild argv from GetCommandLineA() (argv[0] may be quoted). */
static int
parse_cmdline(char ***out)
{
    static char  buf[1024];
    static char *args[32];
    int          n = 0;
    char        *p = buf;

    lstrcpynA(buf, GetCommandLineA(), sizeof buf);
    while (*p && n < 32)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p == '"')
        {
            args[n++] = ++p;
            while (*p && *p != '"')
                p++;
        }
        else
        {
            args[n++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    *out = args;
    return n;
}

int
main(void)
{
    char **argv;
    int    argc = parse_cmdline(&argv);

    /* Make sure every fprintf flushes so we don't lose traces on crash. */
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    int seconds = (argc > 1) ? atoi(argv[1]) : 5;
    if (seconds <= 0)
        seconds = 5;

    fprintf(stderr, "[probe] start, target run = %ds\n", seconds);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return die("CoInitializeEx", hr);

    IPipeASIO *asio = NULL;
    hr              = CoCreateInstance(&CLSID_PipeASIO, NULL, CLSCTX_INPROC_SERVER, &CLSID_PipeASIO,
                                       (void **)&asio);
    if (FAILED(hr) || !asio)
    {
        CoUninitialize();
        return die("CoCreateInstance(CLSID_PipeASIO)", hr);
    }
    fprintf(stderr, "[probe] got IPipeASIO* = %p\n", asio);

    LONG rc = asio->lpVtbl->Init(asio, NULL);
    if (rc != 1)
    { /* IASIO::init returns ASIOTrue (=1) on success */
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return die("Init", rc);
    }

    char name[128] = { 0 };
    asio->lpVtbl->GetDriverName(asio, name);
    LONG ver = asio->lpVtbl->GetDriverVersion(asio);
    fprintf(stderr, "[probe] driver: \"%s\" v%ld\n", name, (long)ver);

    if (argc > 1 && !strcmp(argv[1], "controlpanel"))
    {
        /* Manual handoff test: CP button -> flatpak-spawn / PATH handoff to
         * the native panel, or the built-in Win32 dialog.  Give a spawned
         * panel a moment to appear before the process exits. */
        LONG cprc = asio->lpVtbl->ControlPanel(asio);
        fprintf(stderr, "[probe] ControlPanel -> %ld\n", (long)cprc);
        Sleep(4000);
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return 0;
    }

    LONG nin = 0, nout = 0;
    asio->lpVtbl->GetChannels(asio, &nin, &nout);
    fprintf(stderr, "[probe] channels: %ld in / %ld out\n", (long)nin, (long)nout);

    LONG minBs = 0, maxBs = 0, prefBs = 0, granBs = 0;
    asio->lpVtbl->GetBufferSize(asio, &minBs, &maxBs, &prefBs, &granBs);
    fprintf(stderr, "[probe] buffer sizes: min=%ld max=%ld pref=%ld gran=%ld\n", (long)minBs,
            (long)maxBs, (long)prefBs, (long)granBs);

    double rate = 0.0;
    asio->lpVtbl->GetSampleRate(asio, &rate);
    fprintf(stderr, "[probe] current sample rate: %.0f\n", rate);

    /* CanSampleRate(48000)? if not, fall through with current rate */
    rc = asio->lpVtbl->CanSampleRate(asio, 48000.0);
    if (rc == 0)
    {
        rc = asio->lpVtbl->SetSampleRate(asio, 48000.0);
        if (rc != 0)
        {
            asio->lpVtbl->Release(asio);
            CoUninitialize();
            return die("SetSampleRate(48000)", rc);
        }
        rate = 48000.0;
    }

    /* Allocate BufferInformation for ALL channels (in+out). */
    LONG               nch = nin + nout;
    BufferInformation *bi  = calloc(nch, sizeof *bi);
    if (!bi)
    {
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return 1;
    }
    for (LONG i = 0; i < nin; i++)
    {
        bi[i].isInputType   = 1;
        bi[i].channelNumber = i;
    }
    for (LONG i = 0; i < nout; i++)
    {
        bi[nin + i].isInputType   = 0;
        bi[nin + i].channelNumber = i;
    }

    Callbacks cbs = {
        .swapBuffers             = cb_swapBuffers,
        .sampleRateChanged       = cb_sampleRateChanged,
        .sendNotification        = cb_sendNotification,
        .swapBuffersWithTimeInfo = cb_swapBuffersWithTimeInfo,
    };

    rc = asio->lpVtbl->CreateBuffers(asio, bi, nch, prefBs, &cbs);
    if (rc != 0)
    {
        free(bi);
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return die("CreateBuffers", rc);
    }
    fprintf(stderr, "[probe] CreateBuffers OK (%ld channels @ %ld frames)\n", (long)nch,
            (long)prefBs);

    LONG inLat = 0, outLat = 0;
    asio->lpVtbl->GetLatencies(asio, &inLat, &outLat);
    fprintf(stderr, "[probe] latencies: in=%ld out=%ld\n", (long)inLat, (long)outLat);

    rc = asio->lpVtbl->Start(asio);
    if (rc != 0)
    {
        asio->lpVtbl->DisposeBuffers(asio);
        free(bi);
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return die("Start", rc);
    }
    fprintf(stderr, "[probe] Start OK, running for %d s...\n", seconds);

    /* Timecode was removed in 1.0.0: both selectors must be denied. */
    LONG f_en      = asio->lpVtbl->Future(asio, 1, NULL);  /* kAsioEnableTimeCodeRead */
    LONG f_can     = asio->lpVtbl->Future(asio, 11, NULL); /* kAsioCanTimeCode */
    int  future_ok = (f_en == -998 && f_can == -998);
    fprintf(stderr, "[probe] Future timecode: enable=%ld can=%ld -> %s\n", (long)f_en, (long)f_can,
            future_ok ? "denied (ok)" : "UNEXPECTED");

    /* Count process cycles for N seconds. */
    for (int t = 0; t < seconds; t++)
    {
        LONG before = g_cycles;
        Sleep(1000);
        LONG delta = g_cycles - before;
        fprintf(stderr, "[probe]   t=%d: cycles total=%ld, +%ld this second\n", t + 1,
                (long)g_cycles, (long)delta);
    }

    /* hi stays zero in a short run; lo must advance. */
    w_int64_t spos = { 0, 0 }, stamp = { 0, 0 };
    rc          = asio->lpVtbl->GetSamplePosition(asio, &spos, &stamp);
    int spos_ok = (rc == 0 && spos.hi == 0 && spos.lo > 0);
    fprintf(stderr, "[probe] GetSamplePosition: rc=%ld hi=%lu lo=%lu -> %s\n", (long)rc,
            (unsigned long)spos.hi, (unsigned long)spos.lo, spos_ok ? "ok" : "BAD");

    asio->lpVtbl->Stop(asio);
    fprintf(stderr, "[probe] Stop OK, total cycles = %ld\n", (long)g_cycles);

    asio->lpVtbl->DisposeBuffers(asio);
    asio->lpVtbl->Release(asio);
    free(bi);
    CoUninitialize();

    /* Lower-bound pass criterion; run32.sh is stricter in practice. */
    LONG expected = (LONG)((rate / prefBs) * seconds);
    LONG ok       = (g_cycles >= expected / 2) && future_ok && spos_ok;
    fprintf(stderr, "[probe] expected ~%ld cycles, got %ld -> %s\n", (long)expected, (long)g_cycles,
            ok ? "PASS" : "FAIL");
    return ok ? 0 : 2;
}
