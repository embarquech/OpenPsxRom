Internals
=========

Source layout
-------------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - File
     - Role
   * - ``src/header.s``
     - cartridge header, pre-boot code (switch, banks), shell hook
   * - ``src/crt.s``
     - start-up of the menu in RAM, BIOS call wrappers, EXE trampoline
   * - ``src/main.c``
     - menu, screens, boot log, options, cheats, settings
   * - ``src/gfx.c``
     - GPU: font, icons, gradients, double buffer, screenshot
   * - ``src/protocol.c`` / ``src/sio.c``
     - serial protocol in the menu / SIO1 driver
   * - ``src/flash.c``
     - flash identification and writing (sector and page chips, two chips)
   * - ``src/mcard.c`` / ``src/store.c`` / ``src/lz.c``
     - memory cards / save store / OMC1 compression
   * - ``src/cd.c`` / ``src/boot.c`` / ``src/exe.c``
     - CD driver and ISO9660 / disc boot / EXE start
   * - ``src/strplay.c``, ``src/str/``
     - STR video player (MDEC decoder from PSn00bSDK, MPL-2.0)
   * - ``src/romx.c``
     - code executed from the cartridge while a game runs (debug commands, PCDRV, crash screen,
       in-game menu, code finder)
   * - ``core/``
     - resident core copied to RAM when a game starts
   * - ``tools/``
     - image packing, boot disc, compression, screenshots, emulator scripts

Boot sequence
-------------

1. The BIOS calls the pre-boot entry (``[1F000080h]``) before the kernel is ready.
2. Switch OFF: bypass, or tail-jump to the bank 2/3 firmware (checked). Switch ON: OpenPsxRom
   installs a COP0 execution breakpoint on the shell entry (80030000h) with the debug vector
   (80000040h) pointing to its hook, and returns.
3. When the kernel reaches the shell, the breakpoint fires: the hook copies the unpacker to RAM,
   unpacks the OMC1 payload to 801A0000h and starts the menu there.
4. Booting a disc: SYSTEM.CNF, ``SetConf``, resident core if needed, the executable started through
   a small trampoline that clears its BSS.

Memory map while a game runs
----------------------------

.. list-table::
   :header-rows: 1

   * - Address
     - Content
   * - 8000C000h (3 KB)
     - resident core
   * - 8000CC00h
     - code finder candidates
   * - 8000D800h
     - shared state (flags, cheats, serial window, halt state, speed)
   * - 8000DC00h-8000DEEFh
     - stack of the ROM-executed code
   * - 8000DF00h-8000DFFFh
     - trampoline, breakpoint stack

Resident core
-------------

Kept small (3 KB, in unused kernel space): it registers a handler in the kernel's exception chain
(``SysEnqIntRP`` priority 0) called on every exception and interrupt: video mode forcing, cheats,
Xplorer switch (off then on opens the in-game menu), serial bytes passed to the ROM handler, and
``break`` exceptions (PCDRV, debug halt) handled before the BIOS. It also hooks the BIOS putchar
(TTY) and the unresolved-exception handler (crash screen). Everything big runs from the cartridge
ROM (``src/romx.c``), which has no writable data: its state lives in the shared block.

Flash driver
------------

Commands go through KSEG1 (BF000000h, uncached). The chip is identified with the JEDEC ID sequence
(and the Atmel/SST alternative), with a 10 ms pause for Atmel chips. Page chips are written 128 or
256 bytes at a time after the unlock sequence (which also enables software data protection),
sector chips are erased then programmed byte by byte with DQ7 polling. Identical pages are skipped,
every block is read back and retried. On double-chip carts the commands for 40000h-5FFFFh go to the
second chip. The EXP1 bus timings set by the BIOS are kept (longer ones make the Xplorer drop off
the bus).
