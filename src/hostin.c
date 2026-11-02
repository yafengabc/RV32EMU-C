/*
 * hostin.c - see hostin.h.
 *
 * Windows: a console handle is drained with ReadConsoleInputA (keys only), a
 * pipe with PeekNamedPipe + ReadFile, a redirected file with a plain ReadFile.
 * POSIX: select() with a zero timeout, then read(). Nothing here changes the
 * host's terminal mode, so it is safe to call from the machine loop.
 */
#include "hostin.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

static size_t push_ascii(uint8_t *buf, size_t n, size_t cap, char c) {
    if (c == 0 || n >= cap) return n;
    buf[n++] = (uint8_t)c;
    return n;
}

#ifdef _WIN32

size_t host_stdin_poll(uint8_t *buf, size_t cap) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (h == NULL || h == INVALID_HANDLE_VALUE || cap == 0) return 0;

    if (GetConsoleMode(h, &mode)) {
        DWORD avail = 0;
        if (!GetNumberOfConsoleInputEvents(h, &avail) || avail == 0) return 0;
        INPUT_RECORD recs[32];
        DWORD got = 0;
        if (!ReadConsoleInputA(h, recs, 32, &got)) return 0;
        size_t n = 0;
        for (DWORD i = 0; i < got && n < cap; i++) {
            if (recs[i].EventType != KEY_EVENT) continue;
            if (!recs[i].Event.KeyEvent.bKeyDown) continue;
            n = push_ascii(buf, n, cap, recs[i].Event.KeyEvent.uChar.AsciiChar);
        }
        return n;
    }

    if (GetFileType(h) == FILE_TYPE_PIPE) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail == 0) return 0;
        DWORD want = (DWORD)(avail < cap ? avail : cap);
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, NULL)) return 0;
        return (size_t)got;
    }

    /* redirected file (or anything else that will not block) */
    DWORD got = 0;
    DWORD want = (DWORD)(cap > 65536 ? 65536 : cap);
    if (!ReadFile(h, buf, want, &got, NULL)) return 0;
    return (size_t)got;
}

#else

size_t host_stdin_poll(uint8_t *buf, size_t cap) {
    if (cap == 0) return 0;
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(1, &rfds, NULL, NULL, &tv) <= 0) return 0;
    ssize_t got = read(0, buf, cap);
    return got > 0 ? (size_t)got : 0;
}

#endif
