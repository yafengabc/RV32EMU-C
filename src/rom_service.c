/*
 * ROM service for the ESP32-C3 emulator.
 *
 * The emulator has no real ROM image: the few hundred bytes in rom.c are a
 * hand-built stub.  Firmware from ESP-IDF/MicroPython references the real
 * ROM symbols (from the C3 ROM linker script), so this file implements them
 * in C.  The CPU dispatch is in cpu.c: an illegal instruction whose PC lies
 * in [0x40000000,0x40080000) is routed here and execution resumes at ra.
 *
 * Arguments follow the RISC-V psABI: a0-a7 are x10-x17, further arguments
 * live at sp+0, sp+4, ...  Return values go to a0 (and a1 for 64-bit).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv32.h"

/* ---- ROM symbol addresses (esp-idf esp32c3.rom.ld) ------------------- */

#define ROM_RESET_REASON      0x40000018u
#define ROM_IS_PRINT_BOOT     0x4000003cu
#define ROM_PRINTF            0x40000040u
#define ROM_INSTALL_PUTC1     0x40000044u
#define ROM_INSTALL_UART      0x40000048u
#define ROM_INSTALL_PUTC2     0x4000004cu
#define ROM_DELAY_US          0x40000050u
#define ROM_GET_STACK_INFO    0x40000054u
#define ROM_INSTALL_LOCK      0x40000058u
#define ROM_TX_ONE_CHAR       0x40000068u
#define ROM_TX_ONE_CHAR2      0x4000006cu
#define ROM_RX_ONE_CHAR       0x40000070u
#define ROM_RX_ONE_CHAR_BLOCK 0x40000074u
#define ROM_TX_FLUSH          0x40000080u
#define ROM_TX_WAIT_IDLE      0x40000084u
#define ROM_UART_DIV_MODIFY   0x40000088u
#define ROM_SOFTWARE_RESET    0x40000090u
#define ROM_SOFTWARE_RESET_C  0x40000094u
#define ROM_DISABLE_WD        0x400000a4u
#define ROM_SPIFLASH_IDLE     0x4000010cu
#define ROM_SPIFLASH_CLK      0x40000150u
#define ROM_CACHE_LINE_SIZE   0x400004b0u
#define ROM_BOOT_CACHE_INIT   0x400004c8u
#define ROM_CACHE_INVAL       0x400004d4u
#define ROM_CACHE_DISABLE     0x4000051cu
#define ROM_CACHE_ENABLE      0x40000520u
#define ROM_CACHE_SUSPEND     0x40000524u
#define ROM_CACHE_RESUME      0x40000528u
#define ROM_CACHE_FREEZE_ON   0x4000052cu
#define ROM_CACHE_FREEZE_OFF  0x40000530u
#define ROM_CACHE_IDROM_SIZE  0x40000548u
#define ROM_GET_CPU_FREQ      0x40000584u
#define ROM_SET_CPU_FREQ      0x40000588u
#define ROM_GET_APB_FREQ      0x40000580u
#define ROM_GET_XTAL_FREQ     0x40000598u
#define ROM_GET_XTAL_DIV      0x40000590u
#define ROM_SET_XTAL_DIV      0x40000594u
#define ROM_GPIO_MATRIX_IN    0x400005a0u
#define ROM_GPIO_MATRIX_OUT   0x400005a4u
#define ROM_GPIO_PAD_PULLUP   0x400005c4u
#define ROM_GPIO_PAD_SEL_GPIO 0x400005c8u
#define ROM_GPIO_PAD_SET_DRV  0x400005ccu
#define ROM_INTC_SET_PRIORITY 0x400005e0u
#define ROM_INTC_SET_THRESH   0x400005e4u
#define ROM_INTC_ENABLE       0x400005e8u
#define ROM_INTC_DISABLE      0x400005ecu
#define ROM_INTC_SET_TYPE     0x400005f0u
#define ROM_INTR_MATRIX_SET   0x400005f4u
#define ROM_INTR_LOCK         0x400005f8u
#define ROM_INTR_UNLOCK       0x400005fcu
#define ROM_MD5_INIT          0x40000614u
#define ROM_MD5_UPDATE        0x40000618u
#define ROM_MD5_FINAL         0x4000061cu
#define ROM_CRC32_LE          0x40000628u
#define ROM_CRC32_BE          0x4000062cu
#define ROM_CRC16_LE          0x40000630u
#define ROM_CRC16_BE          0x40000634u
#define ROM_CRC8_LE           0x40000638u
#define ROM_CRC8_BE           0x4000063cu
#define ROM_EFUSE_SPI_CONFIG  0x4000071cu
#define ROM_EFUSE_WP_PAD      0x4000072cu
#define ROM_I2C_READ          0x40001954u
#define ROM_I2C_READ_MASK     0x40001958u
#define ROM_I2C_WRITE         0x4000195cu
#define ROM_I2C_WRITE_MASK    0x40001960u

/* libgcc soft integer helpers */
#define ROM_ADD_DF3           0x4000076cu
#define ROM_ADD_SF3           0x40000770u
#define ROM_ASHLDI3           0x4000077cu
#define ROM_ASHRDI3           0x40000780u
#define ROM_BSWAPDI2          0x40000784u
#define ROM_BSWAPSI2          0x40000788u
#define ROM_CLZSI2            0x4000079cu
#define ROM_CTZSI2            0x400007a8u
#define ROM_DIV_DF3           0x400007b0u
#define ROM_DIVDI3            0x400007b4u
#define ROM_DIV_SF3           0x400007bcu
#define ROM_DIVSI3            0x400007c0u
#define ROM_EQ_DF2            0x400007c4u
#define ROM_EQ_SF2            0x400007c8u
#define ROM_EXTENDSFDF2       0x400007ccu
#define ROM_FFSI2             0x400007d4u
#define ROM_FIXDFSI           0x400007dcu
#define ROM_FIXSFSI           0x400007e4u
#define ROM_FIXUNSDFSI        0x400007e8u
#define ROM_FLOATDIDF         0x400007f4u
#define ROM_FLOATSIDF         0x400007fcu
#define ROM_FLOATSISF         0x40000800u
#define ROM_FLOATUNDIDF       0x40000804u
#define ROM_FLOATUNDISF       0x40000808u
#define ROM_FLOATUNSIDF       0x4000080cu
#define ROM_GEDF2             0x40000818u
#define ROM_GESF2             0x4000081cu
#define ROM_GTDF2             0x40000820u
#define ROM_GTSF2             0x40000824u
#define ROM_LEDF2             0x40000828u
#define ROM_LESF2             0x4000082cu
#define ROM_LSHRDI3           0x40000830u
#define ROM_LTDF2             0x40000834u
#define ROM_LTSF2             0x40000838u
#define ROM_MODDI3            0x4000083cu
#define ROM_MODSI3            0x40000840u
#define ROM_MUL_DF3           0x40000848u
#define ROM_MULDI3            0x4000084cu
#define ROM_MUL_SF3           0x40000854u
#define ROM_MULSI3            0x40000858u
#define ROM_NEDF2             0x40000864u
#define ROM_NEG_DF2           0x40000868u
#define ROM_NEGDI2            0x4000086cu
#define ROM_NEG_SF2           0x40000870u
#define ROM_NESF2             0x4000087cu
#define ROM_SUB_DF3           0x40000894u
#define ROM_SUB_SF3           0x40000898u
#define ROM_TRUNCDFSF2        0x400008a4u
#define ROM_UCMPDI2           0x400008a8u
#define ROM_UDIVDI3           0x400008acu
#define ROM_UDIVMODDI         0x400008b0u
#define ROM_UDIVSI3           0x400008b4u
#define ROM_UMODDI3           0x400008bcu
#define ROM_UMODSI3           0x400008c0u
#define ROM_UNORDDF2          0x400008c4u
#define ROM_UNORDSF2          0x400008c8u

/* ROM libc fallbacks */
#define ROM_MEMSET            0x40000354u
#define ROM_MEMCPY            0x40000358u
#define ROM_MEMMOVE           0x4000035cu
#define ROM_MEMCMP            0x40000360u
#define ROM_STRCPY            0x40000364u
#define ROM_STRNCPY           0x40000368u
#define ROM_STRCMP            0x4000036cu
#define ROM_STRNCMP           0x40000370u
#define ROM_STRLEN            0x40000374u
#define ROM_STRSTR            0x40000378u
#define ROM_BZERO             0x4000037cu
#define ROM_STRCSPN           0x400003E4u
#define ROM_STRSPN            0x40000410u
#define ROM_MEMCHR            0x400003c8u
#define ROM_DIV               0x40000428u
#define ROM_INIT_MTX          0x40000350u
#define ROM_STRCAT            0x400003d8u
#define ROM_STRCHR            0x400003e0u
#define ROM_STRNLEN           0x40000404u
#define ROM_STRRCHR           0x40000408u
#define ROM_STRLCPY           0x400003f0u
#define ROM_QSORT             0x40000434u
#define ROM_UTOA              0x40000444u
#define ROM_ITOA              0x40000448u

/* INTMTX CPU-side register offsets */
#define INTC_ENA_OFF    0x104u
#define INTC_TYPE_OFF   0x108u
#define INTC_PRI_BASE   0x114u
#define INTC_THRESH_OFF 0x194u
#define INTMTX_BASE     0x600C2000u
#define RTC_BASE        0x60008000u
#define UART0_BASE      0x60000000u

#define CALL_SENTINEL 0x4007FFF0u
#define STR_MAX       (1u << 20)

struct rom_service {
    bus_t    *bus;
    uart_t   *uart;
    uint32_t  ticks_per_us;
    uint32_t  xtal_div;
    uint8_t  *unknown;  /* one flag per ROM word (0x80000/4) */
};

/* ---- guest memory helpers ------------------------------------------- */

static uint32_t rd32(rom_service_t *s, uint32_t a) {
    return bus_read(s->bus, a, 4);
}

static void wr32(rom_service_t *s, uint32_t a, uint32_t v) {
    bus_write(s->bus, a, 4, v);
}

static uint8_t rd8(rom_service_t *s, uint32_t a) {
    return (uint8_t)bus_read(s->bus, a, 1);
}

static void wr8(rom_service_t *s, uint32_t a, uint8_t v) {
    bus_write(s->bus, a, 1, v);
}

static bool in_flat(uint32_t a, uint32_t n) {
    return a >= FLAT_BASE && a <= FLAT_BASE + 0x02000000u - n;
}

/* Copy guest memory to/from a host buffer.  Uses the flat map when possible
 * (RAM/flash), and falls back to the bus for XIP windows. */
static void copy_out(rom_service_t *s, uint32_t a, uint8_t *dst, uint32_t n) {
    if (a == 0 || n == 0) return;
    if (in_flat(a, n)) {
        memcpy(dst, s->bus->flat + (a - FLAT_BASE), n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) dst[i] = rd8(s, a + i);
}

static void copy_in(rom_service_t *s, uint32_t a, const uint8_t *src,
                    uint32_t n) {
    if (a == 0 || n == 0) return;
    if (in_flat(a, n)) {
        memcpy(s->bus->flat + (a - FLAT_BASE), src, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) wr8(s, a + i, src[i]);
}

static void fill(rom_service_t *s, uint32_t dst, uint32_t val, uint32_t n) {
    if (n == 0) return;
    if (in_flat(dst, n)) {
        memset(s->bus->flat + (dst - FLAT_BASE), (int)(val & 0xFFu), n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) wr8(s, dst + i, (uint8_t)val);
}

static uint32_t read_cstr(rom_service_t *s, uint32_t addr, char *out,
                          uint32_t max) {
    if (addr == 0) return 0;
    uint32_t n = 0;
    for (; n < max; n++) {
        char c = (char)rd8(s, addr + n);
        if (c == 0) break;
        out[n] = c;
    }
    if (n < max) out[n] = 0;
    else if (max > 0) out[max - 1] = 0;
    return n;
}

/* ---- argument helpers ----------------------------------------------- */

static uint32_t arg(rom_service_t *s, cpu_t *c, int idx) {
    if (idx < 8) return c->x[10 + idx];
    return rd32(s, c->x[2] + (uint32_t)(idx - 8) * 4);
}

static void ret(cpu_t *c, uint32_t v) { c->x[10] = v; }

static uint64_t get_u64(rom_service_t *s, cpu_t *c, int idx) {
    uint64_t lo = arg(s, c, idx);
    uint64_t hi = arg(s, c, idx + 1);
    return lo | (hi << 32);
}

static void set_u64(cpu_t *c, uint64_t v) {
    c->x[10] = (uint32_t)v;
    c->x[11] = (uint32_t)(v >> 32);
}

static double bits_d(uint64_t v) {
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static uint64_t d_bits(double d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

static float bits_f(uint32_t v) {
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static uint32_t f_bits(float f) {
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static uint64_t get_d(rom_service_t *s, cpu_t *c) {
    return get_u64(s, c, 0);
}

static uint64_t get_darg(rom_service_t *s, cpu_t *c, int idx) {
    return get_u64(s, c, idx);
}

static float get_f(rom_service_t *s, cpu_t *c) {
    return bits_f(arg(s, c, 0));
}

static float get_farg(rom_service_t *s, cpu_t *c, int idx) {
    return bits_f(arg(s, c, idx));
}

static void set_d(cpu_t *c, double d) { set_u64(c, d_bits(d)); }
static void set_f(cpu_t *c, float f) { ret(c, f_bits(f)); }

static void cmp_ret(cpu_t *c, bool cond, bool lt_style) {
    uint32_t hit = lt_style ? 0xFFFFFFFFu : 1u;
    uint32_t miss = lt_style ? 1u : 0xFFFFFFFFu;
    ret(c, cond ? hit : miss);
}

static uint32_t bswap32(uint32_t v) {
    return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}

/* ---- console -------------------------------------------------------- */

static void tx(rom_service_t *s, uint8_t b) {
    if (!s->uart || s->uart->discard_tx) return;
    bus_write(s->bus, UART0_BASE, 1, b);
}

static void puts_raw(rom_service_t *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) tx(s, (uint8_t)p[i]);
}

/* ---- printf --------------------------------------------------------- */

static uint32_t u32_to_base(uint32_t v, uint32_t base, bool upper, char *out) {
    char tmp[40];
    uint32_t n = 0;
    do {
        uint32_t d = v % base;
        tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v != 0);
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static void emit_padded(rom_service_t *s, const char *text, uint32_t len,
                        int width, bool zero, bool minus, bool numeric) {
    if (width <= (int)len) {
        puts_raw(s, text, len);
        return;
    }
    uint32_t pad = (uint32_t)width - len;
    if (zero && numeric) {
        if (len > 0 && (text[0] == '-' || text[0] == '+')) {
            tx(s, (uint8_t)text[0]);
            text++;
            len--;
        }
        for (uint32_t i = 0; i < pad; i++) tx(s, '0');
        puts_raw(s, text, len);
    } else if (minus) {
        puts_raw(s, text, len);
        for (uint32_t i = 0; i < pad; i++) tx(s, ' ');
    } else {
        for (uint32_t i = 0; i < pad; i++) tx(s, ' ');
        puts_raw(s, text, len);
    }
}

static void rom_printf(rom_service_t *s, cpu_t *c) {
    uint32_t fmt = arg(s, c, 0);
    if (fmt == 0) return;
    int next = 1;

    char *strbuf = (char *)malloc(4096);
    if (!strbuf) return;

    for (uint32_t i = 0;;) {
        uint8_t ch = rd8(s, fmt + i);
        i++;
        if (ch == 0) break;
        if (ch != '%') {
            tx(s, ch);
            continue;
        }

        bool zero = false, minus = false, plus = false;
        int width = 0;
        for (;;) {
            uint8_t f = rd8(s, fmt + i);
            if (f == '0') {
                zero = true;
                i++;
            } else if (f == '-') {
                minus = true;
                i++;
            } else if (f == '+') {
                plus = true;
                i++;
            } else if (f == ' ' || f == '#') {
                i++;
            } else {
                break;
            }
        }
        for (uint8_t d = rd8(s, fmt + i); d >= '0' && d <= '9';
             d = rd8(s, fmt + i)) {
            width = width * 10 + (int)(d - '0');
            i++;
        }
        int prec = -1;
        if (rd8(s, fmt + i) == '.') {
            i++;
            prec = 0;
            for (uint8_t d = rd8(s, fmt + i); d >= '0' && d <= '9';
                 d = rd8(s, fmt + i)) {
                prec = prec * 10 + (int)(d - '0');
                i++;
            }
        }
        while (rd8(s, fmt + i) == 'l' || rd8(s, fmt + i) == 'h' ||
               rd8(s, fmt + i) == 'z' || rd8(s, fmt + i) == 'j' ||
               rd8(s, fmt + i) == 't') {
            i++;
        }
        uint8_t spec = rd8(s, fmt + i);
        i++;

        char numbuf[48];
        const char *text = numbuf;
        uint32_t len = 0;
        bool numeric = false;

        switch (spec) {
        case 'd':
        case 'i': {
            numeric = true;
            int32_t v = (int32_t)arg(s, c, next++);
            if (v < 0) {
                numbuf[0] = '-';
                len = 1 + u32_to_base((uint32_t)(-(int64_t)v), 10, false,
                                      numbuf + 1);
            } else {
                len = u32_to_base((uint32_t)v, 10, false, numbuf);
                if (plus) {
                    memmove(numbuf + 1, numbuf, len + 1);
                    numbuf[0] = '+';
                    len++;
                }
            }
            break;
        }
        case 'u':
            numeric = true;
            len = u32_to_base(arg(s, c, next++), 10, false, numbuf);
            break;
        case 'x':
            numeric = true;
            len = u32_to_base(arg(s, c, next++), 16, false, numbuf);
            break;
        case 'X':
            numeric = true;
            len = u32_to_base(arg(s, c, next++), 16, true, numbuf);
            break;
        case 'o':
            numeric = true;
            len = u32_to_base(arg(s, c, next++), 8, false, numbuf);
            break;
        case 'p': {
            numeric = true;
            uint32_t v = arg(s, c, next++);
            len = u32_to_base(v, 16, false, numbuf);
            if (len < 8) {
                memmove(numbuf + (8 - len), numbuf, len + 1);
                for (uint32_t k = 0; k < 8 - len; k++) numbuf[k] = '0';
                len = 8;
            }
            break;
        }
        case 'c':
            numbuf[0] = (char)arg(s, c, next++);
            len = 1;
            break;
        case 's': {
            uint32_t p = arg(s, c, next++);
            len = read_cstr(s, p, strbuf, 4096);
            text = strbuf;
            if (prec >= 0 && (int)len > prec) len = (uint32_t)prec;
            break;
        }
        case '%':
            numbuf[0] = '%';
            len = 1;
            break;
        default:
            numbuf[0] = '%';
            numbuf[1] = (char)spec;
            len = 2;
            break;
        }
        emit_padded(s, text, len, width, zero, minus, numeric);
    }
    free(strbuf);
}

/* ---- CRC ------------------------------------------------------------ */

static uint32_t crc32_calc(rom_service_t *s, uint32_t crc, uint32_t p,
                           uint32_t n, bool reflect) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = rd8(s, p + i);
        if (reflect) {
            crc ^= b;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
        } else {
            crc ^= (uint32_t)b << 24;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u
                                          : crc << 1;
            }
        }
    }
    return crc;
}

static uint32_t crc16_calc(rom_service_t *s, uint32_t crc, uint32_t p,
                           uint32_t n, bool reflect) {
    crc &= 0xFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = rd8(s, p + i);
        if (reflect) {
            crc ^= b;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 1) ? (crc >> 1) ^ 0x8408u : crc >> 1;
            }
        } else {
            crc ^= (uint32_t)b << 8;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1;
            }
        }
    }
    return crc & 0xFFFFu;
}

static uint32_t crc8_calc(rom_service_t *s, uint32_t crc, uint32_t p,
                          uint32_t n, bool reflect) {
    crc &= 0xFFu;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = rd8(s, p + i);
        if (reflect) {
            crc ^= b;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 1) ? (crc >> 1) ^ 0x8Cu : crc >> 1;
            }
        } else {
            crc ^= b;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 0x80u) ? (crc << 1) ^ 0x07u : crc << 1;
            }
        }
    }
    return crc & 0xFFu;
}

/* ---- MD5 (RFC 1321) -------------------------------------------------- */

#define MD5_CTX_SIZE  88u
#define MD5_STATE_OFF 0u
#define MD5_COUNT_OFF 16u
#define MD5_BUF_OFF   24u

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const int md5_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(uint32_t st[4], const uint8_t *p) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        f += a + md5_k[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += (f << md5_shift[i]) | (f >> (32 - md5_shift[i]));
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

static void md5_update_local(uint32_t st[4], uint32_t cnt[2], uint8_t buf[64],
                             const uint8_t *data, uint32_t len) {
    uint32_t idx = (cnt[0] >> 3) & 0x3F;
    uint32_t add = len;
    cnt[0] += add << 3;
    if (cnt[0] < (add << 3)) cnt[1]++;
    cnt[1] += add >> 29;

    uint32_t pos = 0;
    if (idx + len < 64) {
        memcpy(buf + idx, data, len);
        return;
    }
    uint32_t part = 64 - idx;
    memcpy(buf + idx, data, part);
    md5_block(st, buf);
    pos = part;
    while (len - pos >= 64) {
        md5_block(st, data + pos);
        pos += 64;
    }
    memcpy(buf, data + pos, len - pos);
}

static void md5_load_ctx(rom_service_t *s, uint32_t addr, uint32_t st[4],
                         uint32_t cnt[2], uint8_t buf[64]) {
    uint8_t raw[MD5_CTX_SIZE];
    copy_out(s, addr, raw, MD5_CTX_SIZE);
    for (int i = 0; i < 4; i++) {
        st[i] = (uint32_t)raw[MD5_STATE_OFF + i * 4] |
                ((uint32_t)raw[MD5_STATE_OFF + i * 4 + 1] << 8) |
                ((uint32_t)raw[MD5_STATE_OFF + i * 4 + 2] << 16) |
                ((uint32_t)raw[MD5_STATE_OFF + i * 4 + 3] << 24);
    }
    cnt[0] = (uint32_t)raw[MD5_COUNT_OFF] |
             ((uint32_t)raw[MD5_COUNT_OFF + 1] << 8) |
             ((uint32_t)raw[MD5_COUNT_OFF + 2] << 16) |
             ((uint32_t)raw[MD5_COUNT_OFF + 3] << 24);
    cnt[1] = (uint32_t)raw[MD5_COUNT_OFF + 4] |
             ((uint32_t)raw[MD5_COUNT_OFF + 5] << 8) |
             ((uint32_t)raw[MD5_COUNT_OFF + 6] << 16) |
             ((uint32_t)raw[MD5_COUNT_OFF + 7] << 24);
    memcpy(buf, raw + MD5_BUF_OFF, 64);
}

static void md5_store_ctx(rom_service_t *s, uint32_t addr, const uint32_t st[4],
                          const uint32_t cnt[2], const uint8_t buf[64]) {
    for (int i = 0; i < 4; i++) wr32(s, addr + MD5_STATE_OFF + (uint32_t)i * 4,
                                     st[i]);
    wr32(s, addr + MD5_COUNT_OFF, cnt[0]);
    wr32(s, addr + MD5_COUNT_OFF + 4, cnt[1]);
    copy_in(s, addr + MD5_BUF_OFF, buf, 64);
}

static void md5_init(rom_service_t *s, uint32_t addr) {
    uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint32_t cnt[2] = {0, 0};
    uint8_t buf[64] = {0};
    md5_store_ctx(s, addr, st, cnt, buf);
}

static void md5_update(rom_service_t *s, uint32_t addr, uint32_t buf_addr,
                       uint32_t len) {
    uint32_t st[4], cnt[2];
    uint8_t buf[64];
    md5_load_ctx(s, addr, st, cnt, buf);
    if (len > 0 && buf_addr != 0) {
        uint8_t *data = (uint8_t *)malloc(len);
        if (!data) return;
        copy_out(s, buf_addr, data, len);
        md5_update_local(st, cnt, buf, data, len);
        free(data);
    }
    md5_store_ctx(s, addr, st, cnt, buf);
}

static void md5_final(rom_service_t *s, uint32_t ctx_addr, uint32_t digest) {
    uint32_t st[4], cnt[2];
    uint8_t buf[64];
    md5_load_ctx(s, ctx_addr, st, cnt, buf);

    uint8_t enc[8];
    enc[0] = (uint8_t)cnt[0];
    enc[1] = (uint8_t)(cnt[0] >> 8);
    enc[2] = (uint8_t)(cnt[0] >> 16);
    enc[3] = (uint8_t)(cnt[0] >> 24);
    enc[4] = (uint8_t)cnt[1];
    enc[5] = (uint8_t)(cnt[1] >> 8);
    enc[6] = (uint8_t)(cnt[1] >> 16);
    enc[7] = (uint8_t)(cnt[1] >> 24);

    int idx = (int)((cnt[0] >> 3) & 0x3F);
    int pad = 56 - idx;
    if (pad <= 0) pad += 64;
    uint8_t padbuf[64];
    memset(padbuf, 0, sizeof(padbuf));
    padbuf[0] = 0x80;
    md5_update_local(st, cnt, buf, padbuf, (uint32_t)pad);
    md5_update_local(st, cnt, buf, enc, 8);

    for (int i = 0; i < 4; i++) {
        wr8(s, digest + (uint32_t)i * 4, (uint8_t)st[i]);
        wr8(s, digest + (uint32_t)i * 4 + 1, (uint8_t)(st[i] >> 8));
        wr8(s, digest + (uint32_t)i * 4 + 2, (uint8_t)(st[i] >> 16));
        wr8(s, digest + (uint32_t)i * 4 + 3, (uint8_t)(st[i] >> 24));
    }
}

/* ---- libc fallbacks ------------------------------------------------- */

static void do_memset(rom_service_t *s, cpu_t *c) {
    uint32_t dst = arg(s, c, 0);
    fill(s, dst, arg(s, c, 1), arg(s, c, 2));
    ret(c, dst);
}

static void do_memcpy(rom_service_t *s, cpu_t *c, bool overlap) {
    uint32_t dst = arg(s, c, 0), src = arg(s, c, 1), n = arg(s, c, 2);
    if (n > 0) {
        if (overlap) {
            uint8_t *tmp = (uint8_t *)malloc(n);
            if (!tmp) {
                ret(c, dst);
                return;
            }
            copy_out(s, src, tmp, n);
            copy_in(s, dst, tmp, n);
            free(tmp);
        } else if (in_flat(src, n) && in_flat(dst, n)) {
            memcpy(s->bus->flat + (dst - FLAT_BASE),
                   s->bus->flat + (src - FLAT_BASE), n);
            bus_touch_code(s->bus, dst);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                wr8(s, dst + i, rd8(s, src + i));
            }
        }
    }
    ret(c, dst);
}

static void do_memcmp(rom_service_t *s, cpu_t *c) {
    uint32_t p0 = arg(s, c, 0), p1 = arg(s, c, 1), n = arg(s, c, 2);
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t a = rd8(s, p0 + i), b = rd8(s, p1 + i);
        if (a != b) {
            r = (a < b) ? 0xFFFFFFFFu : 1u;
            break;
        }
    }
    ret(c, r);
}

static void do_strcpy(rom_service_t *s, cpu_t *c) {
    uint32_t dst = arg(s, c, 0);
    char buf[4096];
    uint32_t n = read_cstr(s, arg(s, c, 1), buf, sizeof(buf));
    copy_in(s, dst, (uint8_t *)buf, n);
    wr8(s, dst + n, 0);
    ret(c, dst);
}

static void do_strncpy(rom_service_t *s, cpu_t *c) {
    uint32_t dst = arg(s, c, 0), src = arg(s, c, 1), n = arg(s, c, 2);
    uint32_t i = 0;
    while (i < n) {
        uint8_t b = rd8(s, src + i);
        wr8(s, dst + i, b);
        i++;
        if (b == 0) break;
    }
    while (i < n) {
        wr8(s, dst + i, 0);
        i++;
    }
    ret(c, dst);
}

static int compare_bytes(const char *a, uint32_t alen, const char *b,
                         uint32_t blen) {
    uint32_t n = alen < blen ? alen : blen;
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static void do_strcmp(rom_service_t *s, cpu_t *c, bool bounded) {
    char a[4096], b[4096];
    uint32_t an = read_cstr(s, arg(s, c, 0), a, sizeof(a));
    uint32_t bn = read_cstr(s, arg(s, c, 1), b, sizeof(b));
    if (bounded) {
        uint32_t n = arg(s, c, 2);
        if (an > n) an = n;
        if (bn > n) bn = n;
    }
    ret(c, (uint32_t)(int32_t)compare_bytes(a, an, b, bn));
}

static void do_strspan(rom_service_t *s, cpu_t *c, bool complement) {
    char str[4096], set[4096];
    uint32_t sn = read_cstr(s, arg(s, c, 0), str, sizeof(str));
    read_cstr(s, arg(s, c, 1), set, sizeof(set));
    uint32_t n = 0;
    while (n < sn) {
        bool inset = strchr(set, str[n]) != NULL;
        if (inset == complement) break;
        n++;
    }
    ret(c, n);
}

static void do_strstr(rom_service_t *s, cpu_t *c) {
    char hay[4096], needle[4096];
    uint32_t hn = read_cstr(s, arg(s, c, 0), hay, sizeof(hay));
    read_cstr(s, arg(s, c, 1), needle, sizeof(needle));
    (void)hn;
    char *p = strstr(hay, needle);
    ret(c, p ? arg(s, c, 0) + (uint32_t)(p - hay) : 0);
}

static void do_memchr(rom_service_t *s, cpu_t *c) {
    uint32_t p = arg(s, c, 0), n = arg(s, c, 2);
    uint8_t ch = (uint8_t)arg(s, c, 1);
    for (uint32_t i = 0; i < n; i++) {
        if (rd8(s, p + i) == ch) {
            ret(c, p + i);
            return;
        }
    }
    ret(c, 0);
}

static void do_strchr(rom_service_t *s, cpu_t *c) {
    uint32_t p = arg(s, c, 0);
    uint8_t ch = (uint8_t)arg(s, c, 1);
    for (uint32_t i = 0;; i++) {
        uint8_t b = rd8(s, p + i);
        if (b == ch) {
            ret(c, p + i);
            return;
        }
        if (b == 0) {
            ret(c, 0);
            return;
        }
    }
}

static void do_strrchr(rom_service_t *s, cpu_t *c) {
    uint32_t p = arg(s, c, 0);
    uint8_t ch = (uint8_t)arg(s, c, 1);
    uint32_t last = 0;
    bool found = false;
    for (uint32_t i = 0;; i++) {
        uint8_t b = rd8(s, p + i);
        if (b == ch) {
            last = p + i;
            found = true;
        }
        if (b == 0) {
            ret(c, found ? last : 0);
            return;
        }
    }
}

static void do_strcat(rom_service_t *s, cpu_t *c) {
    uint32_t dst = arg(s, c, 0), src = arg(s, c, 1);
    uint32_t n = 0;
    for (; n < STR_MAX; n++) {
        if (rd8(s, dst + n) == 0) break;
    }
    char buf[4096];
    uint32_t len = read_cstr(s, src, buf, sizeof(buf));
    copy_in(s, dst + n, (uint8_t *)buf, len);
    wr8(s, dst + n + len, 0);
    ret(c, dst);
}

static void do_itou(rom_service_t *s, cpu_t *c, bool is_unsigned) {
    uint32_t v = arg(s, c, 0);
    uint32_t dst = arg(s, c, 1);
    uint32_t base = arg(s, c, 2);
    if (base < 2 || base > 36) base = 10;
    char digits[40];
    uint32_t n = 0;
    if (!is_unsigned && (int32_t)v < 0) {
        digits[n++] = '-';
        v = (uint32_t)(-(int32_t)v);
    }
    uint32_t start = n;
    do {
        uint32_t d = v % base;
        digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v != 0);
    for (uint32_t a = start, b = n - 1; a < b; a++, b--) {
        char t = digits[a];
        digits[a] = digits[b];
        digits[b] = t;
    }
    copy_in(s, dst, (uint8_t *)digits, n);
    wr8(s, dst + n, 0);
    ret(c, dst);
}

/* ---- guest callbacks (qsort) --------------------------------------- */

static uint32_t call_guest(cpu_t *c, uint32_t fn, uint32_t a0, uint32_t a1) {
    uint32_t saved[32];
    memcpy(saved, c->x, sizeof(saved));
    uint32_t saved_pc = c->pc;
    c->x[10] = a0;
    c->x[11] = a1;
    c->x[1] = CALL_SENTINEL;
    c->pc = fn;
    for (int n = 0; n < 200000; n++) {
        if (c->halted || c->pc == CALL_SENTINEL) break;
        cpu_step(c);
    }
    uint32_t result = c->x[10];
    memcpy(c->x, saved, sizeof(saved));
    c->pc = saved_pc;
    return result;
}

static void do_qsort(rom_service_t *s, cpu_t *c) {
    uint32_t base = arg(s, c, 0), n = arg(s, c, 1), size = arg(s, c, 2);
    uint32_t cmp = arg(s, c, 3);
    if (n < 2 || size == 0 || size > 256 || n > 4096 || cmp == 0) return;
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) return;
    for (uint32_t i = 1; i < n; i++) {
        for (uint32_t j = i; j > 0; j--) {
            uint32_t a = base + (j - 1) * size;
            uint32_t b = base + j * size;
            if ((int32_t)call_guest(c, cmp, a, b) <= 0) break;
            for (uint32_t k = 0; k < size; k++) tmp[k] = rd8(s, a + k);
            for (uint32_t k = 0; k < size; k++) wr8(s, a + k, rd8(s, b + k));
            for (uint32_t k = 0; k < size; k++) wr8(s, b + k, tmp[k]);
        }
    }
    free(tmp);
}

/* ---- dispatch ------------------------------------------------------- */

void rom_service_call(rom_service_t *s, cpu_t *c, uint32_t addr) {
    switch (addr) {
    case ROM_RESET_REASON:
        ret(c, 1); /* POWERON_RESET */
        return;
    case ROM_IS_PRINT_BOOT:
        ret(c, 0);
        return;
    case ROM_PRINTF:
        rom_printf(s, c);
        return;
    case ROM_INSTALL_PUTC1:
    case ROM_INSTALL_UART:
    case ROM_INSTALL_PUTC2:
    case ROM_INSTALL_LOCK:
        ret(c, 0);
        return;
    case ROM_DELAY_US:
        ret(c, 0);
        return;
    case ROM_GET_STACK_INFO:
        ret(c, 0);
        return;
    case ROM_TX_ONE_CHAR:
    case ROM_TX_ONE_CHAR2:
        tx(s, (uint8_t)arg(s, c, 0));
        ret(c, 0);
        return;
    case ROM_RX_ONE_CHAR:
        ret(c, 1); /* no data */
        return;
    case ROM_RX_ONE_CHAR_BLOCK:
        ret(c, 0);
        return;
    case ROM_TX_FLUSH:
    case ROM_TX_WAIT_IDLE:
    case ROM_UART_DIV_MODIFY:
        ret(c, 0);
        return;
    case ROM_SOFTWARE_RESET:
    case ROM_SOFTWARE_RESET_C:
        wr32(s, RTC_BASE, 1u << 31); /* SW_SYS_RST */
        ret(c, 0);
        return;
    case ROM_DISABLE_WD:
        ret(c, 0);
        return;
    case ROM_SPIFLASH_IDLE:
    case ROM_SPIFLASH_CLK:
        ret(c, 0);
        return;
    case ROM_CACHE_LINE_SIZE:
        ret(c, 32);
        return;
    case ROM_BOOT_CACHE_INIT:
    case ROM_CACHE_INVAL:
    case ROM_CACHE_DISABLE:
    case ROM_CACHE_ENABLE:
    case ROM_CACHE_SUSPEND:
    case ROM_CACHE_RESUME:
    case ROM_CACHE_FREEZE_ON:
    case ROM_CACHE_FREEZE_OFF:
    case ROM_CACHE_IDROM_SIZE:
        ret(c, 0);
        return;
    case ROM_GET_CPU_FREQ:
        ret(c, s->ticks_per_us);
        return;
    case ROM_SET_CPU_FREQ:
        s->ticks_per_us = arg(s, c, 0);
        ret(c, 0);
        return;
    case ROM_GET_APB_FREQ:
        ret(c, s->ticks_per_us);
        return;
    case ROM_GET_XTAL_FREQ:
        ret(c, 40);
        return;
    case ROM_GET_XTAL_DIV:
        if (s->xtal_div == 0) s->xtal_div = 1;
        ret(c, s->xtal_div);
        return;
    case ROM_SET_XTAL_DIV:
        s->xtal_div = arg(s, c, 0);
        ret(c, 0);
        return;
    case ROM_GPIO_MATRIX_IN:
    case ROM_GPIO_MATRIX_OUT:
    case ROM_GPIO_PAD_PULLUP:
    case ROM_GPIO_PAD_SEL_GPIO:
    case ROM_GPIO_PAD_SET_DRV:
        ret(c, 0);
        return;
    case ROM_INTR_MATRIX_SET:
        wr32(s, INTMTX_BASE + arg(s, c, 1) * 4, arg(s, c, 2));
        ret(c, 0);
        return;
    case ROM_INTC_SET_PRIORITY:
        wr32(s, INTMTX_BASE + INTC_PRI_BASE + arg(s, c, 0) * 4,
             arg(s, c, 1));
        ret(c, 0);
        return;
    case ROM_INTC_SET_THRESH:
        wr32(s, INTMTX_BASE + INTC_THRESH_OFF, arg(s, c, 0));
        ret(c, 0);
        return;
    case ROM_INTC_ENABLE:
    case ROM_INTC_DISABLE: {
        uint32_t v = rd32(s, INTMTX_BASE + INTC_ENA_OFF);
        if (addr == ROM_INTC_ENABLE) v |= arg(s, c, 0);
        else v &= ~arg(s, c, 0);
        wr32(s, INTMTX_BASE + INTC_ENA_OFF, v);
        ret(c, 0);
        return;
    }
    case ROM_INTC_SET_TYPE: {
        uint32_t v = rd32(s, INTMTX_BASE + INTC_TYPE_OFF);
        uint32_t bit = 1u << (arg(s, c, 0) & 31);
        if (arg(s, c, 1) != 0) v |= bit;
        else v &= ~bit;
        wr32(s, INTMTX_BASE + INTC_TYPE_OFF, v);
        ret(c, 0);
        return;
    }
    case ROM_INTR_LOCK:
    case ROM_INTR_UNLOCK:
        ret(c, 0);
        return;

    case ROM_MD5_INIT:
        md5_init(s, arg(s, c, 0));
        ret(c, 0);
        return;
    case ROM_MD5_UPDATE:
        md5_update(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2));
        ret(c, 0);
        return;
    case ROM_MD5_FINAL:
        md5_final(s, arg(s, c, 1), arg(s, c, 0));
        ret(c, 0);
        return;

    case ROM_CRC32_LE:
        ret(c, crc32_calc(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2), true));
        return;
    case ROM_CRC32_BE:
        ret(c, crc32_calc(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2), false));
        return;
    case ROM_CRC16_LE:
        ret(c, crc16_calc(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2), true));
        return;
    case ROM_CRC16_BE:
        ret(c, crc16_calc(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2), false));
        return;
    case ROM_CRC8_LE:
        ret(c, crc8_calc(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2), true));
        return;
    case ROM_CRC8_BE:
        ret(c, crc8_calc(s, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2), false));
        return;
    case ROM_EFUSE_SPI_CONFIG:
        ret(c, 0);
        return;
    case ROM_EFUSE_WP_PAD:
        ret(c, 0xFFFFFFFFu);
        return;
    case ROM_I2C_READ:
    case ROM_I2C_READ_MASK:
    case ROM_I2C_WRITE:
    case ROM_I2C_WRITE_MASK:
        ret(c, 0);
        return;

    /* ---- software integer helpers --------------------------------- */
    case ROM_ASHLDI3:
        set_u64(c, get_u64(s, c, 0) << (arg(s, c, 2) & 63));
        return;
    case ROM_ASHRDI3: {
        uint32_t sh = arg(s, c, 2) & 63;
        uint64_t v = get_u64(s, c, 0);
        set_u64(c, (uint64_t)((int64_t)v >> sh));
        return;
    }
    case ROM_BSWAPSI2:
        ret(c, bswap32(arg(s, c, 0)));
        return;
    case ROM_BSWAPDI2: {
        uint64_t v = get_u64(s, c, 0);
        set_u64(c, (uint64_t)bswap32((uint32_t)(v >> 32)) |
                       ((uint64_t)bswap32((uint32_t)v) << 32));
        return;
    }
    case ROM_LSHRDI3:
        set_u64(c, get_u64(s, c, 0) >> (arg(s, c, 2) & 63));
        return;
    case ROM_CLZSI2: {
        uint32_t v = arg(s, c, 0), n = 32;
        while (v != 0) {
            n--;
            v >>= 1;
        }
        ret(c, n);
        return;
    }
    case ROM_CTZSI2: {
        uint32_t v = arg(s, c, 0), n = 32;
        if (v != 0) {
            n = 0;
            while ((v & 1) == 0) {
                n++;
                v >>= 1;
            }
        }
        ret(c, n);
        return;
    }
    case ROM_FFSI2: {
        uint32_t v = arg(s, c, 0);
        if (v == 0) {
            ret(c, 0);
        } else {
            uint32_t n = 0;
            while ((v & 1) == 0) {
                n++;
                v >>= 1;
            }
            ret(c, n + 1);
        }
        return;
    }
    case ROM_MULSI3:
        ret(c, arg(s, c, 0) * arg(s, c, 1));
        return;
    case ROM_MULDI3:
        set_u64(c, get_u64(s, c, 0) * get_u64(s, c, 2));
        return;
    case ROM_NEGDI2:
        set_u64(c, ~get_u64(s, c, 0) + 1u);
        return;
    case ROM_UCMPDI2: {
        uint64_t a = get_u64(s, c, 0), b = get_u64(s, c, 2);
        ret(c, a < b ? 0 : (a == b ? 1 : 2));
        return;
    }
    case ROM_UDIVDI3: {
        uint64_t a = get_u64(s, c, 0), b = get_u64(s, c, 2);
        set_u64(c, b == 0 ? 0 : a / b);
        return;
    }
    case ROM_UMODDI3: {
        uint64_t a = get_u64(s, c, 0), b = get_u64(s, c, 2);
        set_u64(c, b == 0 ? 0 : a % b);
        return;
    }
    case ROM_UDIVMODDI: {
        uint64_t a = get_u64(s, c, 0), b = get_u64(s, c, 2);
        uint32_t rem = arg(s, c, 4);
        if (b == 0) {
            set_u64(c, 0);
            wr32(s, rem, 0);
            wr32(s, rem + 4, 0);
        } else {
            uint64_t q = a / b, r = a % b;
            set_u64(c, q);
            wr32(s, rem, (uint32_t)r);
            wr32(s, rem + 4, (uint32_t)(r >> 32));
        }
        return;
    }
    case ROM_DIVDI3: {
        int64_t a = (int64_t)get_u64(s, c, 0), b = (int64_t)get_u64(s, c, 2);
        set_u64(c, b == 0 ? 0 : (uint64_t)(a / b));
        return;
    }
    case ROM_MODDI3: {
        int64_t a = (int64_t)get_u64(s, c, 0), b = (int64_t)get_u64(s, c, 2);
        set_u64(c, b == 0 ? 0 : (uint64_t)(a % b));
        return;
    }
    case ROM_UDIVSI3: {
        uint32_t a = arg(s, c, 0), b = arg(s, c, 1);
        ret(c, b == 0 ? 0 : a / b);
        return;
    }
    case ROM_UMODSI3: {
        uint32_t a = arg(s, c, 0), b = arg(s, c, 1);
        ret(c, b == 0 ? 0 : a % b);
        return;
    }
    case ROM_DIVSI3: {
        int32_t a = (int32_t)arg(s, c, 0), b = (int32_t)arg(s, c, 1);
        ret(c, b == 0 ? 0 : (uint32_t)(a / b));
        return;
    }
    case ROM_MODSI3: {
        int32_t a = (int32_t)arg(s, c, 0), b = (int32_t)arg(s, c, 1);
        ret(c, b == 0 ? 0 : (uint32_t)(a % b));
        return;
    }

    /* ---- software floating point ----------------------------------- */
    case ROM_ADD_DF3:
        set_d(c, bits_d(get_d(s, c)) + bits_d(get_darg(s, c, 2)));
        return;
    case ROM_SUB_DF3:
        set_d(c, bits_d(get_d(s, c)) - bits_d(get_darg(s, c, 2)));
        return;
    case ROM_MUL_DF3:
        set_d(c, bits_d(get_d(s, c)) * bits_d(get_darg(s, c, 2)));
        return;
    case ROM_DIV_DF3:
        set_d(c, bits_d(get_d(s, c)) / bits_d(get_darg(s, c, 2)));
        return;
    case ROM_NEG_DF2:
        set_d(c, -bits_d(get_d(s, c)));
        return;
    case ROM_ADD_SF3:
        set_f(c, get_f(s, c) + get_farg(s, c, 1));
        return;
    case ROM_SUB_SF3:
        set_f(c, get_f(s, c) - get_farg(s, c, 1));
        return;
    case ROM_MUL_SF3:
        set_f(c, get_f(s, c) * get_farg(s, c, 1));
        return;
    case ROM_DIV_SF3:
        set_f(c, get_f(s, c) / get_farg(s, c, 1));
        return;
    case ROM_NEG_SF2:
        set_f(c, -get_f(s, c));
        return;
    case ROM_EQ_DF2:
        ret(c, bits_d(get_d(s, c)) == bits_d(get_darg(s, c, 2)) ? 0 : 1);
        return;
    case ROM_EQ_SF2:
        ret(c, get_f(s, c) == get_farg(s, c, 1) ? 0 : 1);
        return;
    case ROM_NEDF2:
        ret(c, bits_d(get_d(s, c)) != bits_d(get_darg(s, c, 2)) ? 1 : 0);
        return;
    case ROM_NESF2:
        ret(c, get_f(s, c) != get_farg(s, c, 1) ? 1 : 0);
        return;
    case ROM_LTDF2:
        cmp_ret(c, bits_d(get_d(s, c)) < bits_d(get_darg(s, c, 2)), true);
        return;
    case ROM_LEDF2:
        cmp_ret(c, bits_d(get_d(s, c)) <= bits_d(get_darg(s, c, 2)), true);
        return;
    case ROM_GTDF2:
        cmp_ret(c, bits_d(get_d(s, c)) > bits_d(get_darg(s, c, 2)), false);
        return;
    case ROM_GEDF2:
        cmp_ret(c, bits_d(get_d(s, c)) >= bits_d(get_darg(s, c, 2)), false);
        return;
    case ROM_LTSF2:
        cmp_ret(c, get_f(s, c) < get_farg(s, c, 1), true);
        return;
    case ROM_LESF2:
        cmp_ret(c, get_f(s, c) <= get_farg(s, c, 1), true);
        return;
    case ROM_GTSF2:
        cmp_ret(c, get_f(s, c) > get_farg(s, c, 1), false);
        return;
    case ROM_GESF2:
        cmp_ret(c, get_f(s, c) >= get_farg(s, c, 1), false);
        return;
    case ROM_UNORDDF2: {
        double d = bits_d(get_d(s, c));
        ret(c, d != d ? 1 : 0);
        return;
    }
    case ROM_UNORDSF2: {
        float f = get_f(s, c);
        ret(c, f != f ? 1 : 0);
        return;
    }
    case ROM_TRUNCDFSF2:
        set_f(c, (float)bits_d(get_d(s, c)));
        return;
    case ROM_EXTENDSFDF2:
        set_d(c, (double)get_f(s, c));
        return;
    case ROM_FIXDFSI:
        ret(c, (uint32_t)(int32_t)bits_d(get_d(s, c)));
        return;
    case ROM_FIXSFSI:
        ret(c, (uint32_t)(int32_t)get_f(s, c));
        return;
    case ROM_FIXUNSDFSI: {
        double v = bits_d(get_d(s, c));
        if (v < 0 || v != v) ret(c, 0);
        else if (v >= 4294967296.0) ret(c, 0xFFFFFFFFu);
        else ret(c, (uint32_t)v);
        return;
    }
    case ROM_FLOATSIDF:
        set_d(c, (double)(int32_t)arg(s, c, 0));
        return;
    case ROM_FLOATSISF:
        set_f(c, (float)(int32_t)arg(s, c, 0));
        return;
    case ROM_FLOATDIDF:
        set_d(c, (double)(int64_t)get_u64(s, c, 0));
        return;
    case ROM_FLOATUNSIDF:
        set_d(c, (double)arg(s, c, 0));
        return;
    case ROM_FLOATUNDIDF:
        set_d(c, (double)get_u64(s, c, 0));
        return;
    case ROM_FLOATUNDISF:
        set_f(c, (float)get_u64(s, c, 0));
        return;

    /* ---- libc ------------------------------------------------------ */
    case ROM_INIT_MTX:
        ret(c, 0);
        return;
    case ROM_DIV: {
        int32_t a = (int32_t)arg(s, c, 0), b = (int32_t)arg(s, c, 1);
        if (b == 0) {
            c->x[10] = 0;
            c->x[11] = 0;
        } else {
            c->x[10] = (uint32_t)(a / b);
            c->x[11] = (uint32_t)(a % b);
        }
        return;
    }
    case ROM_MEMSET:
        do_memset(s, c);
        return;
    case ROM_MEMCPY:
        do_memcpy(s, c, false);
        return;
    case ROM_MEMMOVE:
        do_memcpy(s, c, true);
        return;
    case ROM_MEMCMP:
        do_memcmp(s, c);
        return;
    case ROM_STRCPY:
        do_strcpy(s, c);
        return;
    case ROM_STRNCPY:
        do_strncpy(s, c);
        return;
    case ROM_STRCMP:
        do_strcmp(s, c, false);
        return;
    case ROM_STRNCMP:
        do_strcmp(s, c, true);
        return;
    case ROM_STRLEN: {
        char buf[4096];
        ret(c, read_cstr(s, arg(s, c, 0), buf, sizeof(buf)));
        return;
    }
    case ROM_STRSPN:
        do_strspan(s, c, false);
        return;
    case ROM_STRCSPN:
        do_strspan(s, c, true);
        return;
    case ROM_STRNLEN: {
        char buf[4096];
        ret(c, read_cstr(s, arg(s, c, 0), buf, arg(s, c, 1)));
        return;
    }
    case ROM_STRLCPY: {
        char buf[4096];
        uint32_t size = arg(s, c, 2);
        uint32_t n = read_cstr(s, arg(s, c, 1), buf, sizeof(buf));
        if (size > 0) {
            uint32_t cpy = n;
            if (cpy > size - 1) cpy = size - 1;
            copy_in(s, arg(s, c, 0), (uint8_t *)buf, cpy);
            wr8(s, arg(s, c, 0) + cpy, 0);
        }
        ret(c, n);
        return;
    }
    case ROM_STRSTR:
        do_strstr(s, c);
        return;
    case ROM_BZERO:
        fill(s, arg(s, c, 0), 0, arg(s, c, 1));
        ret(c, 0);
        return;
    case ROM_MEMCHR:
        do_memchr(s, c);
        return;
    case ROM_STRCHR:
        do_strchr(s, c);
        return;
    case ROM_STRRCHR:
        do_strrchr(s, c);
        return;
    case ROM_STRCAT:
        do_strcat(s, c);
        return;
    case ROM_QSORT:
        do_qsort(s, c);
        return;
    case ROM_ITOA:
    case ROM_UTOA:
        do_itou(s, c, addr == ROM_UTOA);
        return;
    default:
        if (addr >= ROM_BASE && addr < ROM_BASE + ROM_SIZE) {
            uint32_t idx = (addr - ROM_BASE) / 4;
            if (!s->unknown[idx]) {
                s->unknown[idx] = 1;
                fprintf(stderr,
                        "[rom] unimplemented 0x%08X a0=0x%08X a1=0x%08X "
                        "a2=0x%08X ra=0x%08X\n",
                        addr, arg(s, c, 0), arg(s, c, 1), arg(s, c, 2),
                        c->x[1]);
            }
        }
        ret(c, 0);
        return;
    }
}

/* ---- construction --------------------------------------------------- */

rom_service_t *rom_service_new(bus_t *bus, uart_t *uart) {
    rom_service_t *s = (rom_service_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->bus = bus;
    s->uart = uart;
    s->ticks_per_us = 80;
    s->unknown = (uint8_t *)calloc(ROM_SIZE / 4, 1);
    if (!s->unknown) {
        free(s);
        return NULL;
    }
    return s;
}

void rom_service_free(rom_service_t *s) {
    if (!s) return;
    free(s->unknown);
    free(s);
}

/* ---- ROM data layout table ----------------------------------------- */

static const uint32_t rom_layout[40] = {
    0x3FCDC704, 0x3FCDF060, 0x3FCDC710, 0x3FCDE710, 0x00000000, 0x00000000,
    0x3FCDF09C, 0x3FCDF0A8, 0x3FCDF0A8, 0x3FCDF0D0, 0x400591FC, 0x40059200,
    0x3FCDF96C, 0x3FCDFA28, 0x3FCDFA28, 0x3FCDFFD0, 0x3FCDF300, 0x3FCDF5BC,
    0x3FCDF0D0, 0x3FCDF0D4, 0x3FCDF0D4, 0x3FCDF0D8, 0x3FCDF0D8, 0x3FCDF300,
    0x3FCDF834, 0x3FCDF834, 0x3FCDF834, 0x3FCDF84C, 0x3FCDF84C, 0x3FCDF84C,
    0x3FCDF84C, 0x3FCDF870, 0x3FCDF870, 0x3FCDF870, 0x3FCDF870, 0x3FCDF96C,
    0x3FCDF060, 0x3FCDF060, 0x3FCDF060, 0x3FCDF09C,
};

void rom_service_load_rom_data(bus_t *bus) {
    bus_add_ram(bus, ROM_DATA_BASE, ROM_DATA_SIZE);
    for (uint32_t i = 0; i < 40; i++) {
        bus_write(bus, ROM_LAYOUT_ADDR + i * 4, 4, rom_layout[i]);
    }
    bus_write(bus, ROM_LAYOUT_PTR_ADDR, 4, ROM_LAYOUT_ADDR);
}

