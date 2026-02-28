/*
 * AArch64 entry point for the CCR2004 Linux loader.
 *
 *  - Exception vector table (diagnose faults)
 *  - Stay at boot EL (EL2 or EL1) – no automatic drop
 *  - Identity-mapped page table + MMU + caches
 *  - BSS clear, stack, then call loader_main()
 *  - Three kernel boot paths: EL1, EL2, EL2→EL1 drop
 */

#define CURRENTEL_EL2	(2 << 2)	/* CurrentEL value for EL2 */

/* =====================================================================
 * Exception vector table – 16 entries, each 128 bytes, 2KB-aligned.
 * ===================================================================== */

	.macro ventry id
	.balign 128
	mov	x0, #\id
	b	exception_common
	.endm

	.section ".vectors", "ax"
	.balign 2048
	.global exception_vectors
exception_vectors:
	/* Current EL, SP_EL0 (t) */
	ventry  0
	ventry  1
	ventry  2
	ventry  3

	/* Current EL, SP_ELx (h) – this is what we run in */
	ventry  4
	ventry  5
	ventry  6
	ventry  7

	/* Lower EL, AArch64 */
	ventry  8
	ventry  9
	ventry 10
	ventry 11

	/* Lower EL, AArch32 */
	ventry 12
	ventry 13
	ventry 14
	ventry 15

/* ----- common handler: EL-aware register reads, then C dump ----- */
exception_common:
	/* x0 = vector ID (set by ventry macro) */
	mrs	x4, CurrentEL
	cmp	x4, #CURRENTEL_EL2
	b.eq	1f

	/* EL1 path */
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	b	2f

	/* EL2 path */
1:	mrs	x1, esr_el2
	mrs	x2, elr_el2
	mrs	x3, far_el2

2:	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	bl	exception_dump		/* C handler – never returns */
3:	wfe
	b	3b

/* =====================================================================
 * Entry point – stay at whatever EL we're in
 * ===================================================================== */

	.section ".text.boot", "ax"
	.global _start
	.type   _start, %function

_start:
	msr	daifset, #0xf		/* mask all interrupts */

	/* Set up stack (SP_ELx at current EL) */
	ldr	x0, =_stack_top
	mov	sp, x0

	mrs	x0, CurrentEL
	cmp	x0, #CURRENTEL_EL2
	b.eq	_start_el2

/* ----- already at EL1 ----- */
_start_el1:
	adr	x0, exception_vectors
	msr	vbar_el1, x0
	isb
	b	_setup

/* ----- at EL2: stay here, but prepare EL1 for potential later drop ----- */
_start_el2:
	adr	x0, exception_vectors
	msr	vbar_el2, x0
	isb

	/* EL1 is AArch64 (needed if we later drop to EL1) */
	mov	x0, #(1 << 31)		/* HCR_EL2.RW */
	msr	hcr_el2, x0

	/* SCTLR_EL1: sane defaults for potential later use */
	mov	x0, #0x0800
	movk	x0, #0x30D0, lsl #16
	msr	sctlr_el1, x0
	isb
	b	_setup

/* =====================================================================
 * _setup: runs at boot EL with stack ready
 * ===================================================================== */
_setup:
	/* ---------- ensure MMU/caches are OFF at current EL ---------- */
	mrs	x9, CurrentEL
	cmp	x9, #CURRENTEL_EL2
	b.eq	.Ldis_el2

	mrs	x0, sctlr_el1
	bic	x0, x0, #1		/* M – MMU     */
	bic	x0, x0, #4		/* C – D-cache */
	bic	x0, x0, #0x1000		/* I – I-cache */
	msr	sctlr_el1, x0
	isb
	b	.Ldis_done

.Ldis_el2:
	mrs	x0, sctlr_el2
	bic	x0, x0, #1
	bic	x0, x0, #4
	bic	x0, x0, #0x1000
	msr	sctlr_el2, x0
	isb

.Ldis_done:
	/* Clean + invalidate dcache (bootloader may have dirty lines) */
	bl	clean_dcache_all

	ic	iallu

	mrs	x9, CurrentEL
	cmp	x9, #CURRENTEL_EL2
	b.eq	.Ltlb_el2
	tlbi	vmalle1
	b	.Ltlb_done
.Ltlb_el2:
	tlbi	alle2
.Ltlb_done:
	dsb	sy
	isb

	/* ---------- clear BSS (includes page table area) ---------- */
	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
	cmp	x0, x1
	b.ge	.Lbss_done
.Lbss_loop:
	str	xzr, [x0], #8
	cmp	x0, x1
	b.lt	.Lbss_loop
.Lbss_done:

	/* ---------- MAIR ---------- */
	/*  Attr0 = 0x00 (Device-nGnRnE)
	 *  Attr1 = 0xFF (Normal, Inner/Outer WB-RW-A) */
	ldr	x1, =0xFF00

	mrs	x9, CurrentEL
	cmp	x9, #CURRENTEL_EL2
	b.eq	.Lmair_el2
	msr	mair_el1, x1
	b	.Lmair_done
.Lmair_el2:
	msr	mair_el2, x1
.Lmair_done:

	/* ---------- build identity-mapped page table ---------- */
	/*
	 * Level-1 block descriptors (1 GB each):
	 *   [1:0]   = 0b01  block
	 *   [4:2]   = AttrIndx
	 *   [9:8]   = SH
	 *   [10]    = AF
	 *   [53]    = PXN   (set for device regions)
	 *
	 * Normal:  AttrIndx=1, SH=Inner-Sh, AF=1 -> 0x705
	 * Device:  AttrIndx=0, SH=Outer-Sh, AF=1 -> 0x601, +PXN
	 */
	ldr	x0, =__page_table

	/* entry 0: 0x00000000 – Normal Cacheable */
	ldr	x1, =0x0000000000000705
	str	x1, [x0, #0]

	/* entry 1: 0x40000000 – Normal Cacheable */
	ldr	x1, =0x0000000040000705
	str	x1, [x0, #8]

	/* entry 2: 0x80000000 – Device-nGnRnE, PXN */
	ldr	x1, =0x0020000080000601
	str	x1, [x0, #16]

	/* entry 3: 0xC0000000 – Device-nGnRnE, PXN (UART, GIC) */
	ldr	x1, =0x00200000C0000601
	str	x1, [x0, #24]

	/* ---------- TCR + TTBR ---------- */
	mrs	x9, CurrentEL
	cmp	x9, #CURRENTEL_EL2
	b.eq	.Ltcr_el2

	/* TCR_EL1: T0SZ=32, TG0=4KB, IRGN0/ORGN0=WB-WA,
	 *          SH0=Inner-Shareable, EPD1=1 */
	ldr	x1, =0x00A03520
	msr	tcr_el1, x1
	msr	ttbr0_el1, x0
	b	.Ltcr_done

.Ltcr_el2:
	/* TCR_EL2: T0SZ=32, TG0=4KB, IRGN0/ORGN0=WB-WA,
	 *          SH0=Inner-Shareable, PS=40-bit, RES1 bit 31 */
	ldr	x1, =0x80023520
	msr	tcr_el2, x1
	msr	ttbr0_el2, x0

.Ltcr_done:
	isb

	/* ---------- TLB invalidate + enable MMU ---------- */
	mrs	x9, CurrentEL
	cmp	x9, #CURRENTEL_EL2
	b.eq	.Len_el2

	tlbi	vmalle1
	dsb	sy
	isb
	mrs	x0, sctlr_el1
	orr	x0, x0, #1		/* M */
	orr	x0, x0, #4		/* C */
	orr	x0, x0, #0x1000		/* I */
	msr	sctlr_el1, x0
	b	.Len_done

.Len_el2:
	tlbi	alle2
	dsb	sy
	isb
	mrs	x0, sctlr_el2
	orr	x0, x0, #1		/* M */
	orr	x0, x0, #4		/* C */
	orr	x0, x0, #0x1000		/* I */
	msr	sctlr_el2, x0

.Len_done:
	isb

	/* ---------- hand off to C ---------- */
	bl	loader_main

_halt:
	wfe
	b	_halt

/* =====================================================================
 * boot_kernel_el1(kernel_entry, dtb_pa)
 *   Boot from EL1: clean caches, disable MMU, jump.
 *   x0 = kernel entry,  x1 = DTB physical address
 * ===================================================================== */
	.global boot_kernel_el1
	.type boot_kernel_el1, %function
boot_kernel_el1:
	mov	x19, x0
	mov	x20, x1
	bl	clean_dcache_all
	ic	iallu
	tlbi	vmalle1
	dsb	sy
	isb

	mrs	x4, sctlr_el1
	bic	x4, x4, #1
	bic	x4, x4, #4
	bic	x4, x4, #0x1000
	msr	sctlr_el1, x4
	isb

	mov	x0, x20		/* DTB */
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x19

/* =====================================================================
 * boot_kernel_el2(kernel_entry, dtb_pa)
 *   Boot from EL2 staying at EL2: clean caches, disable MMU, jump.
 *   x0 = kernel entry,  x1 = DTB physical address
 * ===================================================================== */
	.global boot_kernel_el2
	.type boot_kernel_el2, %function
boot_kernel_el2:
	mov	x19, x0
	mov	x20, x1
	bl	clean_dcache_all
	ic	iallu
	tlbi	alle2
	tlbi	vmalle1
	dsb	sy
	isb

	mrs	x4, sctlr_el2
	bic	x4, x4, #1
	bic	x4, x4, #4
	bic	x4, x4, #0x1000
	msr	sctlr_el2, x4
	isb

	/* HCR_EL2.RW=1 so kernel can set up EL1 as AArch64 */
	mov	x4, #(1 << 31)
	msr	hcr_el2, x4
	isb

	mov	x0, x20
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x19

/* =====================================================================
 * boot_kernel_el2_to_el1(kernel_entry, dtb_pa)
 *   Running at EL2, drop to EL1 and enter kernel there.
 *   x0 = kernel entry,  x1 = DTB physical address
 * ===================================================================== */
	.global boot_kernel_el2_to_el1
	.type boot_kernel_el2_to_el1, %function
boot_kernel_el2_to_el1:
	mov	x19, x0
	mov	x20, x1
	bl	clean_dcache_all
	ic	iallu
	tlbi	alle2
	tlbi	vmalle1
	dsb	sy
	isb

	mrs	x4, sctlr_el2
	bic	x4, x4, #1
	bic	x4, x4, #4
	bic	x4, x4, #0x1000
	msr	sctlr_el2, x4
	isb

	/* HCR_EL2.RW = 1 (EL1 is AArch64) */
	mov	x4, #(1 << 31)
	msr	hcr_el2, x4

	/* SCTLR_EL1: RES1 bits, MMU/caches off */
	mov	x4, #0x0800
	movk	x4, #0x30D0, lsl #16
	msr	sctlr_el1, x4

	/* SPSR_EL2: return to EL1h with DAIF masked */
	mov	x4, #0x3c5
	msr	spsr_el2, x4

	/* ELR_EL2: kernel entry point */
	msr	elr_el2, x19
	isb

	mov	x0, x20		/* DTB in x0 */
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	eret

/* =====================================================================
 * clean_dcache_all – iterate set/way, DC CISW
 * clobbers: x0-x12
 * ===================================================================== */
	.global clean_dcache_all
	.type clean_dcache_all, %function
clean_dcache_all:
	dmb	sy
	mrs	x0, clidr_el1
	ubfx	x3, x0, #24, #3
	cbz	x3, 5f
	mov	x8, xzr

1:	add	x2, x8, x8, lsr #1
	lsr	x1, x0, x2
	and	x1, x1, #7
	cmp	x1, #2
	b.lt	4f

	msr	csselr_el1, x8
	isb
	mrs	x1, ccsidr_el1

	and	x2, x1, #7
	add	x2, x2, #4

	ubfx	x4, x1, #3, #10
	clz	w5, w4

	ubfx	x6, x1, #13, #15

2:	mov	x7, x6
3:	lsl	x9, x4, x5
	orr	x11, x8, x9
	lsl	x9, x7, x2
	orr	x11, x11, x9
	dc	cisw, x11
	subs	x7, x7, #1
	b.ge	3b
	subs	x4, x4, #1
	b.ge	2b

4:	add	x8, x8, #2
	cmp	x8, x3, lsl #1
	b.lt	1b

5:	dsb	sy
	isb
	ret
