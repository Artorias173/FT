/* Disposition du homebrew de test : code et données en lecture seule, puis données. */
ENTRY(module_start)
SECTIONS {
    . = 0x81000000;
    .text : { *(.text .text.*) *(.vita.stubs) }
    .rodata : { *(.rodata .rodata.*) }
    .sce.reserved : { *(.sce.reserved) }
    . = ALIGN(0x10000);
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
