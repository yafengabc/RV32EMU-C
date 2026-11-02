// leakrun: leak check driver for rv32emu-c.
//
//   leakrun EMU ELF [-ramdisk RD] [-max N] [-runs R] [-join]
//
// Runs the emulator R times in-process (LoadLibrary + _init/_run/_shutdown),
// reporting working-set growth, committed bytes and handle/GDI/user counts.
// -join gates _shutdown through EnterCriticalSection so FindCollision (Rtlp)
// can catch a heap block that is freed twice.
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void (*init)(void);
    int (*run)(const char *cfg);
    void (*shutdown)(void);
    const char *(*last_error)(void);
} api_t;

static void stats(const char *when) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc,
                         sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    printf("[leak] %-22s ws=%8zu KiB private=%8zu KiB committed=%8zu KiB "
           "handles=%lu gdi=%lu user=%lu\n",
           when, pmc.WorkingSetSize / 1024, pmc.PrivateUsage / 1024,
           pmc.PagefileUsage / 1024, (unsigned long)handles,
           (unsigned long)GetGuiResources(GetCurrentProcess(), 0),
           (unsigned long)GetGuiResources(GetCurrentProcess(), 1));
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: leakrun EMU ELF [-ramdisk RD] [-max N] [-runs R] [-join]\n");
        return 2;
    }
    const char *emu = argv[1], *elf = argv[2], *rd = NULL;
    long max_inst = 0, runs = 5;
    int join = 0;
    for (int i = 3; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-ramdisk")) rd = argv[i + 1];
        else if (!strcmp(argv[i], "-max")) max_inst = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "-runs")) runs = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "-join")) join = 1;
    }
    HMODULE lib = LoadLibraryA(emu);
    if (!lib) {
        fprintf(stderr, "LoadLibrary %s failed %lu\n", emu, GetLastError());
        return 1;
    }
    api_t api;
    api.init = (void (*)(void))(void *)GetProcAddress(lib, "_init");
    api.run = (int (*)(const char *))(void *)GetProcAddress(lib, "_run");
    api.shutdown = (void (*)(void))(void *)GetProcAddress(lib, "_shutdown");
    api.last_error = (const char *(*)(void))(void *)GetProcAddress(lib, "_last_error");
    if (!api.init || !api.run || !api.shutdown) {
        fprintf(stderr, "missing _init/_run/_shutdown\n");
        return 1;
    }
    api.init();
    char cfg[2048];
    snprintf(cfg, sizeof(cfg), "config {\n elf \"%s\"\n%s%s%s max %ld\n quiet\n}\n",
             elf, rd ? " ramdisk \"" : "", rd ? rd : "", rd ? "\"\n" : "",
             max_inst);
    stats("before");
    for (long r = 0; r < runs; r++) {
        int rc = api.run(cfg);
        char tag[64];
        snprintf(tag, sizeof(tag), "after run %ld (rc=%d)", r + 1, rc);
        stats(tag);
        if (api.last_error && rc != 0) {
            const char *e = api.last_error();
            if (e && e[0]) printf("[leak] last error: %s\n", e);
        }
    }
    if (join) {
        CRITICAL_SECTION cs;
        InitializeCriticalSection(&cs);
        EnterCriticalSection(&cs);
        LeaveCriticalSection(&cs);
        DeleteCriticalSection(&cs);
    }
    api.shutdown();
    stats("after shutdown");
    FreeLibrary(lib);
    stats("after FreeLibrary");
    return 0;
}
