/*
 * CCR2004 bare-metal Linux loader
 *
 * Decompresses the embedded xz kernel, patches DTB chosen node,
 * flushes caches, disables MMU, and boots per the AArch64 Linux
 * boot protocol.
 *
 * MMU + caches are enabled by start.S before we get here,
 * so decompression runs at full speed.
 */

#include <stdint.h>
#include <stddef.h>

#include "uart.h"
#include "../libfdt/libfdt.h"
#include "../xz-embedded/xz.h"

/* -----------------------------------------------------------------------
 * Destinations – well above our ELF (loaded at 0x01100000, ~7 MB)
 * ----------------------------------------------------------------------- */
#define DTB_DEST        ((uintptr_t)0x01E00000)   /* 30 MB */
#define DTB_MAX_SIZE    (1u * 1024u * 1024u)      /* 1 MB for patched DTB */
#define KERNEL_DEST     ((uintptr_t)0x02000000)   /* 32 MB, 2 MB aligned */
#define KERNEL_MAX_SIZE (32u * 1024u * 1024u)     /* 32 MB output buf    */

#define ARM64_IMAGE_MAGIC 0x644d5241u  /* "ARM\x64" LE */

static const char bootargs[] =
	"console=ttyS0,115200 console=ttyS1 "
	"earlycon=uart8250,mmio32,0xfd883000,115200n8 earlyprintk";

/* -----------------------------------------------------------------------
 * Symbols from blobs.S / linker script
 * ----------------------------------------------------------------------- */
extern uint8_t _kernel_xz_start[];
extern uint8_t _kernel_xz_end[];
extern uint8_t _dtb_start[];
extern uint8_t _dtb_end[];

/* Assembly boot paths in start.S */
extern void boot_kernel_el1(uintptr_t kernel, uintptr_t dtb)
	__attribute__((noreturn));
extern void boot_kernel_el2(uintptr_t kernel, uintptr_t dtb)
	__attribute__((noreturn));
extern void boot_kernel_el2_to_el1(uintptr_t kernel, uintptr_t dtb)
	__attribute__((noreturn));

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static inline unsigned int get_current_el(void)
{
	unsigned long el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned int)((el >> 2) & 3);
}

/* -----------------------------------------------------------------------
 * Static heap for XZ decoder structures (~32 KB needed, 64 KB available)
 * ----------------------------------------------------------------------- */
static uint8_t xz_heap[65536] __attribute__((aligned(8)));
static size_t  xz_heap_pos;

/* -----------------------------------------------------------------------
 * Bump allocator for XZ Embedded (kmalloc macro -> xz_alloc)
 * ----------------------------------------------------------------------- */
void *xz_alloc(size_t size)
{
	size = (size + 7u) & ~7u;
	if (xz_heap_pos + size > sizeof(xz_heap)) {
		uart_puts("FATAL: xz_heap exhausted\n");
		for (;;) __asm__ volatile("wfe");
	}
	void *p = &xz_heap[xz_heap_pos];
	xz_heap_pos += size;
	return p;
}

/* -----------------------------------------------------------------------
 * Exception handler – called from start.S vector stubs
 * ----------------------------------------------------------------------- */
void __attribute__((noreturn))
exception_dump(uint64_t type, uint64_t esr, uint64_t elr, uint64_t far_addr)
{
	static const char * const vec_names[] = {
		"Sync/SP0",  "IRQ/SP0",  "FIQ/SP0",  "SErr/SP0",
		"Sync/SPx",  "IRQ/SPx",  "FIQ/SPx",  "SErr/SPx",
		"Sync/Lo64", "IRQ/Lo64", "FIQ/Lo64", "SErr/Lo64",
		"Sync/Lo32", "IRQ/Lo32", "FIQ/Lo32", "SErr/Lo32",
	};

	unsigned int el = get_current_el();

	uart_puts("\n\n*** EXCEPTION at EL");
	uart_putdec(el);
	uart_puts(": ");
	if (type < 16)
		uart_puts(vec_names[type]);
	else
		uart_putdec(type);
	uart_puts(" ***\n");

	uart_puts("  ESR : "); uart_puthex(esr);      uart_putchar('\n');
	uart_puts("  ELR : "); uart_puthex(elr);      uart_putchar('\n');
	uart_puts("  FAR : "); uart_puthex(far_addr); uart_putchar('\n');

	uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
	uart_puts("  EC  : "); uart_puthex(ec);
	switch (ec) {
	case 0x00: uart_puts(" (Unknown)");              break;
	case 0x01: uart_puts(" (WFI/WFE trap)");         break;
	case 0x15: uart_puts(" (SVC from AArch64)");     break;
	case 0x16: uart_puts(" (HVC from AArch64)");     break;
	case 0x17: uart_puts(" (SMC from AArch64)");     break;
	case 0x18: uart_puts(" (MSR/MRS trap)");         break;
	case 0x20: uart_puts(" (Instr abort, lower EL)");break;
	case 0x21: uart_puts(" (Instr abort, same EL)"); break;
	case 0x22: uart_puts(" (PC alignment)");         break;
	case 0x24: uart_puts(" (Data abort, lower EL)"); break;
	case 0x25: uart_puts(" (Data abort, same EL)");  break;
	case 0x26: uart_puts(" (SP alignment)");         break;
	case 0x2f: uart_puts(" (SError)");               break;
	case 0x30: uart_puts(" (Bkpt, lower EL)");       break;
	case 0x31: uart_puts(" (Bkpt, same EL)");        break;
	case 0x3c: uart_puts(" (BRK)");                  break;
	default:   uart_puts(" (Other)");                break;
	}
	uart_putchar('\n');

	for (;;)
		__asm__ volatile("wfe");
}

/* -----------------------------------------------------------------------
 * Patch DTB /chosen node: set bootargs, remove stale initrd props.
 * ----------------------------------------------------------------------- */
static void patch_dtb(void *dtb, size_t buf_size)
{
	int err;

	/* Open into a larger buffer so we can add properties */
	err = fdt_open_into(dtb, dtb, (int)buf_size);
	if (err) {
		uart_puts("WARN: fdt_open_into failed: ");
		uart_putdec((unsigned long)-err);
		uart_putchar('\n');
		return;
	}

	int chosen = fdt_path_offset(dtb, "/chosen");
	if (chosen < 0) {
		/* /chosen doesn't exist, create it */
		chosen = fdt_add_subnode(dtb, 0, "chosen");
		if (chosen < 0) {
			uart_puts("WARN: cannot create /chosen: ");
			uart_putdec((unsigned long)-chosen);
			uart_putchar('\n');
			return;
		}
	}

	/* Set bootargs */
	err = fdt_setprop_string(dtb, chosen, "bootargs", bootargs);
	if (err) {
		uart_puts("WARN: fdt_setprop_string(bootargs) failed: ");
		uart_putdec((unsigned long)-err);
		uart_putchar('\n');
	}

	/* Remove stale initrd properties (zeros that confuse the kernel) */
	fdt_delprop(dtb, chosen, "linux,initrd-start");
	fdt_delprop(dtb, chosen, "linux,initrd-end");

	/* Remove empty bootargs-append */
	fdt_delprop(dtb, chosen, "bootargs-append");

	err = fdt_pack(dtb);
	if (err) {
		uart_puts("WARN: fdt_pack failed: ");
		uart_putdec((unsigned long)-err);
		uart_putchar('\n');
	}

	uart_puts("DTB patched, bootargs:\n  ");
	uart_puts(bootargs);
	uart_puts("\n");
}

/* -----------------------------------------------------------------------
 * Ask user which EL to boot the kernel at (only when running at EL2).
 * Returns 1 or 2.
 * ----------------------------------------------------------------------- */
static unsigned int ask_boot_el(void)
{
	uart_puts("Boot kernel at which EL? [1] EL1  [2] EL2 (default): ");

	for (;;) {
		int c = uart_getchar();
		if (c == '1') {
			uart_puts("EL1\n");
			return 1;
		}
		if (c == '2' || c == '\r' || c == '\n') {
			uart_puts("EL2\n");
			return 2;
		}
		/* ignore other keys */
	}
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
void loader_main(void)
{
	uart_init();
	uart_puts("\n\nCCR2004 Linux Loader\n");

	unsigned int boot_el = get_current_el();
	uart_puts("Running at EL"); uart_putdec(boot_el);
	uart_puts(", MMU + D-cache + I-cache enabled\n");

	/* If at EL2, let user choose kernel boot EL */
	unsigned int kernel_el = 1;
	if (boot_el == 2)
		kernel_el = ask_boot_el();

	size_t kxz_size = (size_t)(_kernel_xz_end - _kernel_xz_start);
	size_t dtb_size = (size_t)(_dtb_end        - _dtb_start);

	uart_puts("Kernel.xz: "); uart_puthex((uintptr_t)_kernel_xz_start);
	uart_puts(" ("); uart_putdec(kxz_size); uart_puts(" bytes)\n");

	uart_puts("DTB:       "); uart_puthex((uintptr_t)_dtb_start);
	uart_puts(" ("); uart_putdec(dtb_size); uart_puts(" bytes)\n");

	/* ---- copy DTB and patch /chosen ---- */
	uart_puts("Copying DTB to "); uart_puthex(DTB_DEST); uart_puts("\n");
	memcpy((void *)DTB_DEST, _dtb_start, dtb_size);

	patch_dtb((void *)DTB_DEST, DTB_MAX_SIZE);

	/* ---- decompress kernel ---- */
	uart_puts("Decompressing kernel to "); uart_puthex(KERNEL_DEST); uart_puts("\n");

	xz_crc32_init();
	xz_heap_pos = 0;
	struct xz_dec *xz = xz_dec_init(XZ_SINGLE, 0);
	if (!xz) {
		uart_puts("FATAL: xz_dec_init failed\n");
		for (;;) __asm__ volatile("wfe");
	}

	struct xz_buf b = {
		.in      = _kernel_xz_start,
		.in_pos  = 0,
		.in_size = kxz_size,
		.out     = (uint8_t *)KERNEL_DEST,
		.out_pos = 0,
		.out_size = KERNEL_MAX_SIZE,
	};

	enum xz_ret ret = xz_dec_run(xz, &b);
	if (ret != XZ_STREAM_END) {
		uart_puts("FATAL: xz decompression failed, ret=");
		uart_putdec((unsigned long)ret);
		uart_putchar('\n');
		for (;;) __asm__ volatile("wfe");
	}

	size_t kernel_size = b.out_pos;
	uart_puts("Decompressed "); uart_putdec(kernel_size); uart_puts(" bytes\n");

	xz_dec_end(xz);

	/* ---- validate Image header ---- */
	const uint8_t *img = (const uint8_t *)KERNEL_DEST;
	uint32_t magic;
	memcpy(&magic, img + 56, 4);
	if (magic != ARM64_IMAGE_MAGIC) {
		uart_puts("FATAL: bad Image magic ");
		uart_puthex(magic);
		uart_putchar('\n');
		for (;;) __asm__ volatile("wfe");
	}

	uint64_t text_offset, image_size;
	memcpy(&text_offset, img + 8,  8);
	memcpy(&image_size,  img + 16, 8);

	uart_puts("Image OK: text_offset="); uart_puthex(text_offset);
	uart_puts(", image_size="); uart_puthex(image_size); uart_puts("\n");

	/*
	 * AArch64 boot protocol: Image must be placed at
	 * (2MB-aligned base) + text_offset.  KERNEL_DEST is our
	 * 2MB-aligned base; shift the decompressed Image up by
	 * text_offset so the kernel's position matches its expectation.
	 */
	uintptr_t kernel_entry = KERNEL_DEST + text_offset;
	if (text_offset > 0) {
		uart_puts("Relocating Image to "); uart_puthex(kernel_entry); uart_puts("\n");
		memmove((void *)kernel_entry, (void *)KERNEL_DEST, kernel_size);
	}

	/* ---- boot ---- */
	uart_puts("Booting kernel @ "); uart_puthex(kernel_entry);
	uart_puts(", dtb @ "); uart_puthex(DTB_DEST);
	uart_puts(" [EL"); uart_putdec(kernel_el); uart_puts("]\n");

	for (volatile int i = 0; i < 100000; i++)
		;

	if (boot_el == 2 && kernel_el == 2)
		boot_kernel_el2(kernel_entry, DTB_DEST);
	else if (boot_el == 2 && kernel_el == 1)
		boot_kernel_el2_to_el1(kernel_entry, DTB_DEST);
	else
		boot_kernel_el1(kernel_entry, DTB_DEST);
}
