#pragma once
#include <stdint.h>

/*
 * NS16550A-compatible UART0 on CCR2004.
 *   Base: 0xfd883000, reg-shift=2 (each reg at offset*4), reg-io-width=4
 *   Clock: 500 MHz, target baud: 115200
 *   Divisor = 500000000 / (16 * 115200) = 271
 */

#define UART_BASE       0xfd883000UL
#define UART_REG(n)     (*(volatile uint32_t *)(UART_BASE + (unsigned)(n) * 4u))

#define UART_THR        UART_REG(0)   /* Transmit Holding Register */
#define UART_IER        UART_REG(1)
#define UART_FCR        UART_REG(2)   /* FIFO Control Register    */
#define UART_LCR        UART_REG(3)   /* Line Control Register    */
#define UART_MCR        UART_REG(4)
#define UART_LSR        UART_REG(5)   /* Line Status Register     */
#define UART_DLL        UART_REG(0)   /* Divisor Latch LSB (DLAB=1) */
#define UART_DLM        UART_REG(1)   /* Divisor Latch MSB (DLAB=1) */

#define UART_RBR        UART_REG(0)   /* Receive Buffer Register  */
#define LSR_DR          (1u << 0)     /* Data Ready */
#define LSR_THRE        (1u << 5)     /* Transmit Holding Register Empty */

static inline void uart_init(void)
{
	UART_LCR = 0x83u;   /* DLAB=1, 8-bit, no parity, 1 stop */
	UART_DLL = 271u & 0xffu;
	UART_DLM = (271u >> 8) & 0xffu;
	UART_LCR = 0x03u;   /* DLAB=0, 8N1 */
	UART_FCR = 0xc7u;   /* enable & clear FIFOs, 14-byte trigger */
	UART_IER = 0x00u;   /* disable interrupts */
}

static inline int uart_getchar(void)
{
	while (!(UART_LSR & LSR_DR))
		;
	return (int)(UART_RBR & 0xffu);
}

static inline void uart_putchar(char c)
{
	while (!(UART_LSR & LSR_THRE))
		;
	UART_THR = (uint32_t)(unsigned char)c;
}

static inline void uart_puts(const char *s)
{
	for (; *s; s++) {
		if (*s == '\n')
			uart_putchar('\r');
		uart_putchar(*s);
	}
}

static inline void uart_puthex(unsigned long v)
{
	static const char h[] = "0123456789abcdef";
	uart_puts("0x");
	/* skip leading zeros, but always print at least one digit */
	int started = 0;
	for (int i = 60; i >= 0; i -= 4) {
		int d = (v >> i) & 0xf;
		if (d || started || i == 0) {
			uart_putchar(h[d]);
			started = 1;
		}
	}
}

static inline void uart_putdec(unsigned long v)
{
	char buf[24];
	int i = sizeof(buf) - 1;
	buf[i] = '\0';
	if (v == 0) { uart_putchar('0'); return; }
	while (v && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
	uart_puts(&buf[i]);
}
