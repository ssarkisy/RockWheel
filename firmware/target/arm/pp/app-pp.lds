#define __ASSEMBLER__
#include "cpu.h"

/* Will have been included from app.lds */
ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/pp/crt0-pp.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE

#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGINSIZE - CODECSIZE

#define DRAMORIG 0x00000000
#define IRAMORIG 0x40000000
#define IRAMSIZE 0xc000

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)

/* Where the codec buffer ends, and the plugin buffer starts */
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)

MEMORY
{
    DRAM : ORIGIN = DRAMORIG,     LENGTH = DRAMSIZE
    IRAM : ORIGIN = IRAMORIG,     LENGTH = IRAMSIZE
}

SECTIONS
{
    .text :
    {
        loadaddress = .;
        _loadaddress = .;
        . = ALIGN(0x200);
        *(.init.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
        _textend = .;   /* the code a Stkov record's trace accepts (panic.c) */
    } > DRAM

    .rodata :
    {
        *(.rodata)  /* problems without this, dunno why */
        *(.rodata*)
        *(.rodata.str1.1)
        *(.rodata.str1.4)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
        *(.data*)
        . = ALIGN(0x4);
    } > DRAM

#if NOCACHE_BASE != 0
    /* .ncdata section is placed at uncached physical alias address and is
     * loaded at the proper cached virtual address - no copying is
     * performed in the init code */
    .ncdata . + NOCACHE_BASE :
    {
        . = ALIGN(CACHEALIGN_SIZE);
        *(.ncdata*)
        . = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif
    
    /DISCARD/ :
    {
        *(.eh_frame)
    }

    .vectors 0x0 :
    {
        _vectorsstart = .;
        KEEP(*(.vectors));
        _vectorsend = .;
    } AT> DRAM

    _vectorscopy = LOADADDR(.vectors);
    _noloaddram  = LOADADDR(.vectors);

    .ibss IRAMORIG (NOLOAD) :
    {
        _iedata = .;
        *(.qharray)
        *(.ibss*)
        . = ALIGN(0x4);
        _iend = .;
    } > IRAM

    .iram _iend :
    {
        _iramstart = .;
        *(.icode*)
        *(.irodata*)
        *(.idata*)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM AT> DRAM

    _iramcopy = LOADADDR(.iram);


    .init ENDAUDIOADDR : 
    {
        . = ALIGN(4);
        _initstart = .;
        *(.init*)
        _initend = .;
    } AT> DRAM

    _initcopy = LOADADDR(.init);

    .idle_stacks (NOLOAD) :
    {
       *(.idle_stacks)
       . = ALIGN(8);
#if NUM_CORES > 1
       cpu_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cpu_idlestackend = .;
#endif
       cop_idlestackbegin = .;
       . += IDLE_STACK_SIZE;
       cop_idlestackend = .;
    } > IRAM

    /* .bss and .ncbss are treated as a single section to use one init loop to
     * zero it - note "_edata" and "_end" */
    .bss _noloaddram (NOLOAD) :
    {
       _edata = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
       /* The main thread's stack, zeroed with .bss, then filled by crt0. In
          DRAM, as the 8 KB that fit in IRAM were too few: in an emulator a
          keyboard in a playlist picker in a list's context menu went to 8352
          bytes, and by frame sums a USB connect there reaches about 10 KB.
          16 KB leaves over 6 KB above both. At the end of .bss, as .bss begins
          inside the image's own load span, over which ROLO copies a new image
          while it runs on this stack. In whole cache lines, so that no
          variable that either core writes shares a line with it.
          Below it the crash stacks (crt0-pp.S), whose contents matter only
          once a panic has begun, so that an overflow goes 4 KB before it
          reaches data. Without them it went through about 816 bytes of other
          .bss (the skin parser's state, data_abort_regs, debug's counters)
          into the disk cache's table of sectors, which a write-back before
          the next switch_thread() check would then use. The COP's half, on
          top, is used only by a COP panic. An overflow of the main thread
          less than 2 KB past the bottom has its panic on the CPU's half,
          below; one still under way deeper shares that half with its own
          panic, which may then garble the record's message and trace: only
          the diagnosis suffers. */
       . = ALIGN(CACHEALIGN_SIZE);
       *(.uie_stacks)
       . = ALIGN(CACHEALIGN_SIZE);
       stackbegin = .;
       . += 0x4000;
       stackend = .;
    } > DRAM

#if NOCACHE_BASE != 0
    .ncbss . + NOCACHE_BASE (NOLOAD):
    {
    	. = ALIGN(CACHEALIGN_SIZE);
        *(.ncbss*)
    	. = ALIGN(CACHEALIGN_SIZE);
    } AT> DRAM
#endif

    /* This will be aligned by preceding alignments */
    .endaddr . - NOCACHE_BASE (NOLOAD) :
    {
        _end = .;
    } > DRAM

    .audiobuf _end (NOLOAD) :
    {
        _audiobuffer = .;
        . = ALIGN(0x4);
        audiobuffer = .;
    } > DRAM
    
    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
#ifdef IPOD_VIDEO
        audiobufend_lds = .;
#else
        audiobufend = .;
#endif
        _audiobufend = .;
    } > DRAM

    .codec ENDAUDIOADDR (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
    }

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }
}
