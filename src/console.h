/*
 * console.h - interactive terminal (UART console) for rv32emu-c.
 *
 * console_write() is called by the UART whenever the guest writes the TX FIFO;
 * console_input() is polled by the machine loop and hands bytes to the UART
 * RX FIFO. The console remembers the UART pointer so the two sides stay
 * connected across software resets.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rv32.h"

#define UART_RX_THROTTLE 16   /* bytes accepted per batch while the guest is busy */

/* Start the terminal; returns false if the console resources are missing. */
bool console_start(void);

/* Restore the host console (modes, cursor, window title). Safe to call twice. */
void console_stop(void);

/* Queue bytes printed by the guest (called from the UART THR path). */
void console_write(const char *data, size_t n);

/* Called periodically (timer tick) with the emulator's instruction counter:
 * flushes pending output and decides how many input bytes may be released. */
void console_tick(uint64_t now_ms);

/* Poll the host console, decode keys, and push bytes into the UART RX FIFO
 * through uart_rx_push(). Returns the number of bytes accepted by the guest
 * side, or -1 when the user asked to quit (Ctrl-]). */
int console_input(void);

/* True when the user pressed the quit key. */
bool console_quit_requested(void);

/* Wire the console to a UART instance (uses uart_rx_push). */
void console_set_uart(uart_t *uart);

#endif /* CONSOLE_H */
