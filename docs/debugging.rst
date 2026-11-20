Debugging
=========

All commands use ``nops`` v18 (``/dest COMx``). The link is SIO1 at 115200 bauds, 8N1.

Boot log
--------

Open a terminal on the port and reset the console::

   OpenPsxRom 0.7 (Nov 20 2026 10:00:00) starting
   BIOS: System ROM Version 4.1 12/16/97 E, CRC32 318178BF
   Console: SCPH-7002/7502/9002
   Video: PAL (50 Hz)
   Flash: SST 29EE020 (ID BF/10), 256 KB, ROM CRC 183F4277
   Banks: 1 OpenPsxRom, 2 Unirom, 3 empty
   Started from: ROM bank 1
   Flash @40000h: second chip present (not probed)
   Cheats: 0 codes, saves in flash: 0
   Switch: on, off at power-on = ROM bank 2; firmware in bank 2: yes, bank 3: no
   Pad 1: digital pad
   Pad 2: none
   Card 1: PS1 card, 3 blocks used, 12 free
   Card 2: none
   CD: lid closed, locked
   Serial: 115200 bauds, nops commands accepted
   OpenPsxRom 0.7 ready

* **BIOS**: its version string (none before v2.0) and the CRC32 of the whole 512 KB, the same as
  ``zlib.crc32`` of a dump: compare it with known BIOS dumps.
* **Console**: the models that shipped with this BIOS, from its version and region. One BIOS went
  into several models (4.1 E: SCPH-7002, 7502 and 9002), which nothing in software tells apart.
* **Banks**: what each bank of the cartridge holds (known firmware names, else the text of its
  header). **Started from**: the cartridge (and which build) or an EXE in RAM (boot disc,
  ``nops /exe``).
* **CD**: ``unlocked`` once the drive accepted the unlock of the menu; the drive itself cannot be
  asked, so at power-on this is always ``locked``. ``psx.py info`` (``cd_unlocked``) gives the
  state later.

Load and run code
-----------------

From the menu:

.. code-block:: sh

   nops /exe program.exe /dest COMx              # upload a PS-EXE and start it
   nops /bin data.bin 0x80100000 /dest COMx      # upload raw data (v18: file, then address)
   nops /jmp 0x80100000 /dest COMx               # jump to an address
   nops /dump 0x80010000 0x1000 out.bin /dest COMx
   nops /poke32 0x80100000 0x12345678 /dest COMx

Faster transfers: add ``/psxbaud 230400 /pcbaud 230400`` to a command (``/psxbaud`` switches the
PS1, ``/pcbaud`` the PC; both are needed). The console returns to 115200 after that command, and
after about 3 s without traffic. ``/fast`` (518400) needs an adapter able to do it.

While a program runs
--------------------

.. code-block:: sh

   nops /debug /dest COMx                        # install the resident debug core
   nops /exe program.exe /m /dest COMx           # start it and watch its output

With the debug core (or the *Serial debug in game* and *printf -> serial* boot options for a CD):

* printf/TTY output goes to the serial port;
* ``/dump``, ``/bin``, ``/poke``, ``/exe``, ``/jmp`` work while the program runs;
* ``/halt`` freezes it, ``/cont`` resumes it, ``/regs`` and ``/setreg`` read and write its registers;
* ``/hookex``, ``/hookread``, ``/hookwrite`` set a hardware breakpoint (COP0), ``/unhook`` removes it;
* a crash prints the registers on screen and on the serial port;
* the speed options work as in the menu.

Sent to the menu, the commands that need a running program (``/halt``, ``/cont``, hooks) answer
``ONLY``, as Unirom does.

GDB
---

``nops /gdb`` puts the console in debug mode and starts a GDB server on TCP port 3333. Connect a
MIPS-capable GDB (``gdb-multiarch``)::

   target remote localhost:3333
   break main
   continue
   info registers

The bridge only uses commands OpenPsxRom implements (DEBG, HALT, CONT, DUMP, SBIN, HKEX...);
``break`` instructions halt the program like a software breakpoint. A full GDB session has not
been tested on hardware yet.

PCDRV
-----

Programs can read and write files on the PC through ``nops /m``: ``break`` with code 101h-107h
(PCinit, PCcreat, PCopen, PCclose, PCread, PCwrite, PClseek); arguments in a1-a3, error in v0,
result in v1. Both encodings are accepted: PSn00bSDK (``break 0, 0x101``) and the Sony SDK
(``break 0x101``). The break is caught in the kernel's exception chain, before the BIOS's own break
handling (a development-unit feature that ends in a bus error on retail consoles).

Screenshots
-----------

.. code-block:: sh

   python tools/psxshot.py screen.png --port COMx --nops path/to/nops.exe
   python tools/psxshot.py vram.png --rect 0 0 1024 512 --scale 1 --port COMx --nops ...

The ``SHOT`` command copies the picture on screen (or any VRAM rectangle) to RAM through the GPU;
the script fetches it with ``nops /dump`` and writes a PNG. Works while the menu runs; no TV
needed.

Memory cards
------------

.. code-block:: sh

   nops /mcdown 0 card.mcr /dest COMx            # read slot 1 (raw 128 KB image)
   nops /mcup 0 card.mcr /dest COMx              # write it: ERASES the card

Every frame is read back after writing and rewritten if needed (a card may still be busy with the
previous frame). Reading a card takes several seconds before the transfer starts.

Emulator
--------

``tools/run_duck.ps1`` boots ``build/openrom.rom`` in a DuckStation copy configured for Xplorer
cartridge emulation and prints the TTY log; ``tools/shot.ps1`` captures its window. The ``SHOT``
compile option (``make DEFS=-DSHOT=n``) opens a screen directly, for screenshots without a pad.
