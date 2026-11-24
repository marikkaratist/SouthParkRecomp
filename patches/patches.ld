RAMBASE  = 0x80D81000; /* Used to hold any new symbols: the top 512 KiB of the 14 MiB the game is told it has (src/game/draw_distance.cpp) */
PATCH_RAM_END = 0x80E00000; /* 0x80E00000-0x80F80000 holds copies of the game's VM windows (src/game/boot.cpp) */

MEMORY {
    extram : ORIGIN = RAMBASE, LENGTH = 64M
    rom : ORIGIN = 0, LENGTH = 64M
}

SECTIONS {
    .ctors         : { *(.ctors*) *(.init_array*)                } >extram AT >rom
    .dtors         : { *(.dtors*)                                } >extram AT >rom
    .text          : { *(.text*)                                 } >extram AT >rom
    .recomp_patch  : { *(.recomp_patch*) *(.recomp_force_patch*) } >extram AT >rom
    .recomp_export : { *(.recomp_export*)                        } >extram AT >rom
    .recomp_event  : { *(.recomp_event*)                         } >extram AT >rom
    .rodata        : { *(.rodata*)                               } >extram AT >rom
    .data          : { *(.data*)                                 } >extram AT >rom
    .bss  (NOLOAD) : { *(.bss*)  *(COMMON)                       } >extram
    ASSERT(. <= PATCH_RAM_END, "Maxed out recomp extra ram")
    
    .reloc 0 : { *(.reloc*) }
    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }

    /DISCARD/ : { *(*); }
}