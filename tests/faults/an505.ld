/* QEMU's mps2-an505 (Cortex-M33, starting secure): SSRAM1's secure alias
   at 0x10000000 for the program, SSRAM2's at 0x38000000 for RAM. */
MEMORY {
  FLASH (rx) : ORIGIN = 0x10000000, LENGTH = 4M
  RAM (rwx) : ORIGIN = 0x38000000, LENGTH = 2M
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
