/*
 * Linker script for the CCR2004 Linux loader.
 * Load address: 0x01100000
 *
 * Everything goes into a single .text section so that bootloaders
 * that only load .text will still pick up our read-only data blobs.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x01100000;

    .text : {
        /* exception vectors first (2KB aligned entry point) */
        . = ALIGN(2048);
        KEEP(*(.vectors))

        /* executable code */
        . = ALIGN(8);
        KEEP(*(.text.boot))
        *(.text*)

        /* read-only data, embedded kernel + DTB – all in .text */
        . = ALIGN(8);
        *(.rodata*)
        *(.data*)
    }

    /* ---- zero-initialised (not stored in ELF file) ---- */
    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)

        . = ALIGN(4096);
        __page_table = .;
        . = . + 4096;

        __bss_end = .;
    }

    .stack (NOLOAD) : ALIGN(16) {
        __stack_bottom = .;
        . = . + 0x8000;
        _stack_top = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
