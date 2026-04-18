/*
******************************************************************************
**
**  Bootloader Linker Script for STM32H723ZGT6
**  - Bootloader en FLASH 0x08000000, tamaño 128 KBytes
**  - Stack en DTCMRAM (0x20000000..0x2001FFFF)
**  - .data / .bss en RAM_D1 (0x24000000..0x2404FFFF)
**
******************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack: end of DTCMRAM */
_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);  /* 0x20000000 + 128K = 0x20020000 */

/* Minimal heap/stack sizes (solo para comprobación) */
_Min_Heap_Size  = 0x200;  /* 512 bytes de heap  */
_Min_Stack_Size = 0x400;  /* 1 KB de stack */

/* Specify the memory areas */
MEMORY
{
  /* Bootloader en los primeros 128K de Flash */
  FLASH    (rx)   : ORIGIN = 0x08000000, LENGTH = 128K

  /* RAMs internas del STM32H723 */
  ITCMRAM  (xrw)  : ORIGIN = 0x00000000, LENGTH = 64K
  DTCMRAM  (xrw)  : ORIGIN = 0x20000000, LENGTH = 128K

  /* AXI SRAM (RAM_D1) – 320 KBytes */
  RAM_D1   (xrw)  : ORIGIN = 0x24000000, LENGTH = 320K

  /* D2/D3 SRAM más pequeñas */
  RAM_D2   (xrw)  : ORIGIN = 0x30000000, LENGTH = 32K
  RAM_D3   (xrw)  : ORIGIN = 0x38000000, LENGTH = 16K
}

/* Define output sections */
SECTIONS
{
  /* La tabla de vectores va al principio de FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code / vector table */
    . = ALIGN(4);
  } >FLASH

  /* Código (.text) en FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* end of code */
  } >FLASH

  /* Datos constantes en FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections */
    *(.rodata*)        /* .rodata* sections */
    . = ALIGN(4);
  } >FLASH

  /* Tablas de excepciones en FLASH */
  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  /* Constructores / destructores */
  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* LMA de .data (in FLASH) */
  _sidata = LOADADDR(.data);

  /* .data in RAM_D1, loaded from FLASH */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* data start in RAM */
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)

    . = ALIGN(4);
    _edata = .;        /* data end in RAM */
  } >RAM_D1 AT> FLASH

  /* .bss en RAM_D1 */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;

    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM_D1

  /* Heap + stack (solo marcador, el stack real empieza en _estack en DTCMRAM) */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM_D1

  /* Eliminar símbolos de librerías para minimizar tamaño */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}