/*
 * Embed the xz-compressed kernel and DTB as read-only data.
 * Paths are relative to the project root (where make is invoked).
 */

	.section ".rodata.kernel", "a"
	.balign 4
	.global _kernel_xz_start
_kernel_xz_start:
	.incbin "build/kernel.xz"
	.global _kernel_xz_end
_kernel_xz_end:

	.section ".rodata.dtb", "a"
	.balign 8
	.global _dtb_start
_dtb_start:
	.incbin "build/CCR2004-dt-1.dtb"
	.global _dtb_end
_dtb_end:
