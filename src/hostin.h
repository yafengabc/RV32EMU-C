/*
 * hostin.h - non-blocking read of the host's standard input.
 *
 * The Go emulator always runs a goroutine that copies stdin into the UART's
 * host-side buffer, so scripted sessions (`rv32emu -elf ... < cmds.txt`) work
 * without any special flag. This is the C equivalent: a poll that never blocks
 * and never touches the console state, used when the interactive terminal
 * (-console) is not active.
 */
#ifndef HOSTIN_H
#define HOSTIN_H

#include <stddef.h>
#include <stdint.h>

/* Read whatever is available on stdin without blocking. Returns the number of
 * bytes copied into buf (0 when there is nothing to read). */
size_t host_stdin_poll(uint8_t *buf, size_t cap);

#endif /* HOSTIN_H */
