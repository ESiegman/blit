/* QEMU's mps2-an385: 4 MB of code memory at 0, 4 MB of RAM at 0x20000000. */
MEMORY {
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
