/* QEMU's sifive_e: the program in its flash (XIP) at 0x20400000, where its
   reset vector jumps; 16 KB of RAM (DTIM) at 0x80000000. */
MEMORY {
  FLASH (rx) : ORIGIN = 0x20400000, LENGTH = 512K
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 16K
}
SECTIONS {
  .text : { *(.start) *(.text*) *(.rodata*) *(.srodata*) } > FLASH
  .data : { *(.data*) *(.sdata*) } > RAM AT > FLASH
  .bss : { *(.bss*) *(.sbss*) *(COMMON) } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
