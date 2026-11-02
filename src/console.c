/*
 * console.c - interactive serial terminal for the emulated UART0.
 *
 * The machine loop runs on the main thread (like the Go build); this file
 * owns the host console:
 *
 *   guest -> host : every byte the guest writes to the UART TX FIFO is queued
 *                   here and flushed once per batch, so a guest that prints
 *                   faster than the terminal can absorb cannot stall it.
 *   host -> guest : console_input() polls the console once per batch and
 *                   pushes kept bytes into the UART RX FIFO. Input is handed
 *                   over in small pieces while the guest is busy inside its
 *                   ISR and released in bigger pieces once it is idle, which
 *                   keeps the kernel's 8-line line discipline from dropping
 *                   pasted commands.
 *
 * Host console:
 *   - Windows console: raw input (no line editing/echo), UTF-8 code page and
 *     Virtual Terminal Processing for ANSI output. CRLF is translated so the
 *     guest format (newlines only) renders correctly.
 *   - redirected stdin/stdout: plain byte pump.
 *
 * Keys: Ctrl-C = SIGINT to the guest (byte 0x03), Ctrl-] = quit the emulator.
 */
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* One batch of emulator work is ~1000 instructions; release at most this
 * many bytes per batch while the guest is inside its ISR. */
#define RX_BUSY_BYTES 16

/* ---- ring buffer ----------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t len;
} ring_t;

static bool ring_init(ring_t *r, size_t cap) {
    memset(r, 0, sizeof(*r));
    r->buf = (uint8_t *)malloc(cap);
    if (!r->buf) return false;
    r->cap = cap;
    return true;
}

static size_t ring_put(ring_t *r, const uint8_t *p, size_t n) {
    if (n > r->cap - r->len) n = r->cap - r->len;
    for (size_t i = 0; i < n; i++) {
        r->buf[r->tail] = p[i];
        r->tail = (r->tail + 1) % r->cap;
    }
    r->len += n;
    return n;
}

static size_t ring_get(ring_t *r, uint8_t *p, size_t n) {
    if (n > r->len) n = r->len;
    for (size_t i = 0; i < n; i++) {
        p[i] = r->buf[r->head];
        r->head = (r->head + 1) % r->cap;
    }
    r->len -= n;
    return n;
}

static bool ring_peek(const ring_t *r, size_t at, uint8_t *out) {
    if (at >= r->len) return false;
    *out = r->buf[(r->head + at) % r->cap];
    return true;
}

static void ring_drop(ring_t *r, size_t n) {
    if (n > r->len) n = r->len;
    r->head = (r->head + n) % r->cap;
    r->len -= n;
}

static void ring_free(ring_t *r) {
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

/* ---- state ----------------------------------------------------------- */

static bool g_active;
static bool g_quit;

static bool g_interactive; /* stdin is a real console */

static ring_t g_tx; /* guest -> host */
static ring_t g_rx; /* host -> guest */
static uint64_t g_last_input; /* last time the guest read its RX FIFO */
static int g_rx_probe;        /* bytes the guest had when we last looked */
static bool g_busy;
static size_t g_rx_buflen; /* bytes we pushed but that are still unread */
static int g_msgs;

static uart_t *g_uart;

#ifdef _WIN32
static DWORD g_in_mode;
static DWORD g_out_mode;
static bool g_modes_saved;
static HANDLE g_stdin_h, g_stdout_h;
static CRITICAL_SECTION g_lock;
#endif

/* ---- output ---------------------------------------------------------- */

static void write_raw(const uint8_t *p, size_t n) {
#ifdef _WIN32
    DWORD done = 0;
    while (n > 0) {
        DWORD chunk = (DWORD)(n > (1u << 30) ? (1u << 30) : n);
        if (!WriteFile(g_stdout_h, p, chunk, &done, NULL) || done == 0) return;
        p += done;
        n -= done;
    }
#else
    (void)fwrite(p, 1, n, stdout);
    fflush(stdout);
#endif
}

static void write_str(const char *s) { write_raw((const uint8_t *)s, strlen(s)); }

static void message(const char *s) {
    if (g_interactive) {
        write_str("\r\n\033[2m");
        write_str(s);
        write_str("\033[0m\r\n");
    } else {
        fprintf(stdout, "\n%s\n", s);
        fflush(stdout);
    }
}

/* Guest text uses "\n" only; the Windows console needs CR before it. */
static void emit_guest(const uint8_t *p, size_t n) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\n') continue;
        if (i > start) write_raw(p + start, i - start);
        write_raw((const uint8_t *)"\r\n", 2);
        start = i + 1;
    }
    if (n > start) write_raw(p + start, n - start);
}

static void flush_tx(void) {
    if (g_tx.len == 0) return;
    uint8_t chunk[8192];
    size_t n;
    while ((n = ring_get(&g_tx, chunk, sizeof(chunk))) > 0) emit_guest(chunk, n);
}

/* ---- input ----------------------------------------------------------- */

/* Push a decoded byte to the guest RX FIFO. `counted` bytes are tracked so we
 * can tell whether the guest actually consumed them. */
static bool push_guest_byte(uint8_t b) {
    if (g_uart && uart_rx_push(g_uart, &b, 1) == 1) {
        g_rx_buflen++;
        return true;
    }
    return false;
}

/* Handle escape sequences locally where they do something useful for the
 * user, otherwise forward them (the shell handles arrow keys). */
static void input_byte(uint8_t b) {
    (void)b;
}

/* Feed raw host bytes (console keys or a pipe) into the guest queue. */
static size_t decode_input(const uint8_t *p, size_t n) {
    size_t queued = 0;
    size_t i = 0;
    while (i < n) {
        if (g_rx.len >= g_rx.cap) break;
        uint8_t b = p[i];
        if (b == 0x1d) { /* Ctrl-] */
            g_quit = true;
            i++;
            continue;
        }
        if (b == 0x0d) { /* CR -> newline for the guest */
            uint8_t nl = '\n';
            queued += ring_put(&g_rx, &nl, 1);
            i++;
            if (i < n && p[i] == '\n') i++; /* tolerate CRLF */
            continue;
        }
        queued += ring_put(&g_rx, &b, 1);
        i++;
        input_byte(b);
    }
    return queued;
}

/* Console key events (Windows). */
#ifdef _WIN32
static void poll_console_keys(void) {
    if (!g_interactive) return;
    DWORD avail = 0;
    if (!GetNumberOfConsoleInputEvents(g_stdin_h, &avail) || avail == 0) return;
    INPUT_RECORD recs[64];
    DWORD got = 0;
    DWORD want = avail > 64 ? 64 : avail;
    if (!ReadConsoleInputA(g_stdin_h, recs, want, &got)) return;
    EnterCriticalSection(&g_lock);
    for (DWORD i = 0; i < got; i++) {
        INPUT_RECORD *r = &recs[i];
        if (r->EventType != KEY_EVENT || !r->Event.KeyEvent.bKeyDown) continue;
        WORD vk = r->Event.KeyEvent.wVirtualKeyCode;
        DWORD ctl = r->Event.KeyEvent.dwControlKeyState;
        char ch = r->Event.KeyEvent.uChar.AsciiChar;
        bool ctrl = (ctl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        if (ctrl && vk == 'C') {
            uint8_t c = 0x03;
            (void)ring_put(&g_rx, &c, 1);
            continue;
        }
        if (ctrl && vk == VK_OEM_6) { /* Ctrl-] */
            g_quit = true;
            continue;
        }
        /* Quit keys that work regardless of the keyboard layout: some hosts
         * never deliver Ctrl-] (US layout key ']' is VK_OEM_6, other layouts
         * put it somewhere else), so Ctrl-K is accepted as well. */
        if (ctrl && vk == 'K') {
            g_quit = true;
            continue;
        }
        if (ctrl && vk == 'D') {
            uint8_t c = 0x04;
            (void)ring_put(&g_rx, &c, 1);
            continue;
        }
        static const char *seq[4] = {"\033[A", "\033[B", "\033[C", "\033[D"};
        if (vk == VK_UP) (void)ring_put(&g_rx, (const uint8_t *)seq[0], 3);
        else if (vk == VK_DOWN) (void)ring_put(&g_rx, (const uint8_t *)seq[1], 3);
        else if (vk == VK_RIGHT) (void)ring_put(&g_rx, (const uint8_t *)seq[2], 3);
        else if (vk == VK_LEFT) (void)ring_put(&g_rx, (const uint8_t *)seq[3], 3);
        else if (vk == VK_RETURN) {
            uint8_t nl = '\n';
            (void)ring_put(&g_rx, &nl, 1);
        } else if (vk == VK_BACK) {
            uint8_t del = 0x7f;
            (void)ring_put(&g_rx, &del, 1);
        } else if (ch) {
            /* the console reported an ASCII/UTF-8 byte (localized keyboard) */
            (void)ring_put(&g_rx, (const uint8_t *)&ch, 1);
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void poll_pipe(void) {
    if (g_interactive) return;
    uint8_t buf[4096];
    DWORD avail = 0;
    if (GetFileType(g_stdin_h) == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(g_stdin_h, NULL, 0, NULL, &avail, NULL) || avail == 0) return;
    } else if (!avail) {
        return;
    }
    DWORD got = 0;
    if (!ReadFile(g_stdin_h, buf, sizeof(buf), &got, NULL) || got == 0) return;
    EnterCriticalSection(&g_lock);
    (void)decode_input(buf, got);
    LeaveCriticalSection(&g_lock);
}
#endif

/* ---- public API ------------------------------------------------------ */

void console_set_uart(uart_t *uart) { g_uart = uart; }

bool console_start(void) {
    if (g_active) return true;
    if (!ring_init(&g_tx, 512 * 1024)) return false;
    if (!ring_init(&g_rx, 64 * 1024)) return false;
    g_quit = false;
    g_last_input = UINT64_MAX;
    g_busy = false;
    g_rx_buflen = 0;
    g_rx_probe = 0;
    g_msgs = 0;
    g_active = true;


#ifdef _WIN32
    InitializeCriticalSection(&g_lock);
    g_stdin_h = GetStdHandle(STD_INPUT_HANDLE);
    g_stdout_h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_interactive = GetConsoleMode(g_stdin_h, &mode) != 0;
    if (g_interactive) {
        g_in_mode = mode;
        GetConsoleMode(g_stdout_h, &g_out_mode);
        g_modes_saved = true;
        /* raw keys, no echo, no line buffering, no Ctrl-C from the console */
        SetConsoleMode(g_stdin_h, (mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                            ENABLE_PROCESSED_INPUT)) |
                                      ENABLE_WINDOW_INPUT);
        SetConsoleMode(g_stdout_h, g_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCtrlHandler(NULL, TRUE); /* Ctrl-C belongs to the guest */
    }
#endif
    message("rv32emu terminal: Ctrl-C = SIGINT to the guest, Ctrl-] or Ctrl-K = quit");
    flush_tx();
    return true;
}

void console_stop(void) {
    if (!g_active) return;
    flush_tx();
#ifdef _WIN32
    if (g_interactive && g_modes_saved) {
        SetConsoleMode(g_stdin_h, g_in_mode);
        SetConsoleMode(g_stdout_h, g_out_mode);
        SetConsoleCtrlHandler(NULL, FALSE);
    }
    DeleteCriticalSection(&g_lock);
#endif
    ring_free(&g_tx);
    ring_free(&g_rx);
    g_active = false;
}

void console_write(const char *data, size_t n) {
    if (!g_active || !data || n == 0) return;
    if (g_tx.cap - g_tx.len < n) {
        /* host terminal is slower than the guest: drop, but say so once */
        if (g_msgs < 3) {
            g_msgs++;
            message("[console: output dropped, the host terminal is too slow]");
        }
        return;
    }
    (void)ring_put(&g_tx, (const uint8_t *)data, n);
}

void console_tick(uint64_t now_ms) {
    if (!g_active) return;
    (void)now_ms;
    flush_tx();
}

bool console_quit_requested(void) { return g_quit; }

int console_input(void) {
    if (!g_active) return 0;
#ifdef _WIN32
    if (g_interactive) {
        EnterCriticalSection(&g_lock);
        poll_console_keys();
        LeaveCriticalSection(&g_lock);
    }
    poll_pipe();
#else
    {
        uint8_t buf[4096];
        ssize_t got = read(0, buf, sizeof(buf));
        if (got > 0) (void)decode_input(buf, (size_t)got);
    }
#endif
    if (g_quit) return -1;

    /* How many bytes the guest still has in its FIFO? If it is below what we
     * pushed, the guest has consumed some and is ready for more. */
    if (g_uart) {
        uint32_t pending = uart_rx_pending(g_uart);
        if ((size_t)pending < g_rx_buflen) g_rx_buflen = pending;
        if (pending == 0 && g_rx.len > 0) g_busy = false;
        (void)g_rx_probe;
    }

    size_t limit = g_busy ? RX_BUSY_BYTES : (size_t)-1;
    size_t sent = 0;
    uint8_t b;
    while (g_rx.len > 0 && sent < limit) {
        /* keep escape sequences together: never split right after ESC */
        if (!ring_peek(&g_rx, 0, &b)) break;
        if (b == 0x1b) {
            uint8_t n1 = 0;
            if (!ring_peek(&g_rx, 1, &n1)) break;
            if (n1 == '[') {
                size_t j = 2;
                uint8_t c = 0;
                while (ring_peek(&g_rx, j, &c) && !(c >= 0x40 && c <= 0x7e)) j++;
                if (j >= g_rx.len) break; /* incomplete sequence, wait */
                if (g_rx.len < j + 1) break;
            } else if (g_rx.len < 2) {
                break;
            }
        }
        if (!push_guest_byte(b)) {
            g_busy = true;
            break;
        }
        ring_drop(&g_rx, 1);
        sent++;
    }
    (void)input_byte;
    return (int)sent;
}



