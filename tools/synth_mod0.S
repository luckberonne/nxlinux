/* MOD0 for a flat-linked guest main compiled with tools/synth_guest.ld: the
   module block their loader's module_dyn_from_mod0 reads to find the dynamic
   section. Everything here is a plain constant (no relocations in the block):
   the block itself is pinned by the script at the data segment's start, and the
   pointer word at image+4 spells that address out. */
.section .text.mod0, "ax"
.balign 4
.global mod0_start
mod0_start:
  .word 0                        /* image+0: free (the pointer word sits at +4) */
  .word mod0_blk - mod0_start    /* image+4: the block's address; the linker resolves the difference (a text of any size fits) */
.section .text

.section .module.mod0blk, "aw"   /* the block: the script makes it first in .data */
mod0_blk:
  .word 0x30444f4d               /* "MOD0" */
  .word 8                        /* the dynamic section: right after the block */
