/* A flat link for a guest main built as an NSO: text at 0, ro and data each on
   their own 0x1000 page (an NSO has no relocations layout to hide gaps in, and
   memory offsets hand prelinked addresses their exact link-time vaddrs back).
   lld only keeps this paging when the PHDRS are declared, and -shared (the way
   both a symtab and data relocations reach the packed image) with
   -z max-page-size=0x1000. The MOD0 emitted by tools/synth_mod0.S must come
   first in text, with the dynamic section first in the data segment so the
   block can reach it. Link: clang --target=aarch64-linux-musl -shared -nostdlib
   -nostartfiles -fuse-ld=lld -Wl,-T,tools/synth_guest.ld,-z,max-page-size=0x1000
   -o guest.elf guest.c tools/synth_mod0.S */

ENTRY(mod0_start)
PHDRS
{
  text PT_LOAD FLAGS(5);   /* R + X */
  ro   PT_LOAD FLAGS(4);   /* R */
  data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
  . = 0;
  .text :
  {
    KEEP(*(.text.mod0))            /* the MOD0 block, must be first */
    . = ALIGN(0x10);
    *(.text*)
  } :text

  . = ALIGN(0x1000);
  .rodata :
  {
    *(.rodata*) *(.eh_frame*) *(.note*)
  } :ro
  .rela.dyn  : { *(.rela.dyn) *(.rela.dyn.*) } :ro   /* relocations belong to the image, the loader applies them */
  .dynsym    : { *(.dynsym) } :ro                    /* .dynamic names these */
  .dynstr    : { *(.dynstr) } :ro
  .gnu.version    : { *(.gnu.version) } :ro
  .gnu.version_r  : { *(.gnu.version_r) } :ro

  . = ALIGN(0x1000);
  .module.mod0blk 0x2000 :
  {
    KEEP(*(.module.mod0blk))     /* the MOD0 block, at 0x2000 (tools/synth_mod0.S spells it out) */
  } :data
  .dynamic :
  {
    dyn_start = .;               /* the dynamic section: right after the block */
    KEEP(*(.dynamic))
  } :data
  .data :
  {
    *(.data*)
  } :data
  .bss : { *(.bss*) *(COMMON) } :data   /* zero-filled: an NSO has one bss size for the data segment */
}
