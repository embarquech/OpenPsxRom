Supported hardware
==================

Cartridges
----------

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Cartridge
     - Flash
     - Notes
   * - Xplorer / Xploder V1-V3, Classic, Pro
     - SST 29EE020 (256 KB)
     - Tested. OpenPsxRom in bank 1, a second firmware in bank 2.
   * - Xplorer FX, double chip
     - 2 x SST 29EE020
     - Second chip at 1F040000h; 384 KB addressable, the Xplorer registers sit at 1F060000h.
       Bank 3 used on hardware. The second chip is never sent a command outside a write (seen
       once without its software data protection: every command byte was then stored as data,
       and the ID probe at each boot wiped it). Bank 3 is written like the others
       (``python tools/psx.py flash`` or ``nops /rom`` with an image built for 1F040000h; tested
       with Caetla 0.38, read back identical). Fallback: ``make flash3 IMG=...``
       (``build/flash3.exe``, run with ``nops /exe``), which writes and reads back 128 KB without
       screen or interrupts.
   * - Xplorer FX, single 512 KB chip
     - Winbond W29C040 (reported on forums)
     - Not tested. Unknown chips are reported with their ID in the boot log.
   * - Datel (Action Replay / PS Hacker)
     - AMD Am29F010 (128 KB)
     - One bank. Some sectors may be hardware-protected (12 V needed to unprotect); with sectors
       0 and 5 locked, see below.

Known flash chips: AMD Am29F010, ST M29F010B, Fujitsu MBM29F010, SST 39SF010A/020A/040,
SST 29EE010/29EE020/29LE020, Atmel AT29C010A/020/040A, Winbond W29EE011.

Sector protection is only shown for AMD-style chips (AMD, ST, Fujitsu), the only ones that have
it. Atmel, SST and Winbond page-write chips use software data protection, handled by the driver.

The Xplorer switch and the banks
--------------------------------

The flash is mapped linearly from 1F000000h. OpenPsxRom calls each 128 KB a *bank*:

.. list-table::
   :header-rows: 1

   * - Bank
     - Address
     - Content
   * - 1
     - 1F000000h-1F01FFFFh
     - OpenPsxRom (64 KB) + saves, cheats, settings
   * - 2
     - 1F020000h-1F03FFFFh
     - optional second firmware (e.g. Unirom "ROM2", built for this address)
   * - 3
     - 1F040000h-1F05FFFFh
     - double-chip Xplorer FX only: second chip

At power-on the BIOS calls the cartridge's pre-boot entry. OpenPsxRom reads the Xplorer switch
(bit 0 of 1F060000h):

* switch **ON**: OpenPsxRom starts;
* switch **OFF**: what *Boot options > Switch OFF at power-on* says: **bypass** (default: the
  cartridge stays invisible, normal boot), **ROM bank 2** or **ROM bank 3**. Before jumping, the
  pre-boot code checks the firmware's "Licensed by Sony" header and that its entry point lies inside
  its bank; otherwise it falls back to bypass, so an empty or damaged bank never blocks the boot.

A firmware is position-dependent: only images built for 1F020000h (or 1F040000h) can run from bank
2 (or 3). Caetla only exists for bank 1: ``tools/caetla_reloc.py`` relocates it (0.34, 0.37, 0.38
tested) to bank 2 or 3. It unpacks Caetla's compressed RAM modules to move their references too,
and points its Action Replay port reads (which fall inside bank 2 on an Xplorer) at erased flash::

   python tools/caetla_reloc.py caetla.bin 2 build/caetla_bank2.rom   # then nops /rom
   python tools/caetla_reloc.py caetla.bin 3 build/caetla_bank3.rom   # then psx.py flash

Do not save Caetla's settings from a relocated copy: its own flash routines address the chip as in
bank 1 and would write over OpenPsxRom's settings page. Unirom's own installer always writes bank 1: use ``nops /rom`` with OpenPsxRom, which
detects the target bank (see :doc:`installing`).

Serial cable
------------

SIO1, 115200 bauds 8N1, 3.3 V levels. PS1 serial port pins: 1 RXD (to the adapter's TX), 4 TXD
(to the adapter's RX), 5 CTS (the PS1 only transmits when the PC raises RTS), 7 GND. Pin 2 is
+3.5 V: do not connect it to the adapter.

Datel cart with locked sectors
------------------------------

Some Datel carts ("PS Hacker (Version 1999)", Am29F010) have sectors 0 and 5 hardware-protected:
the BIOS entries and Datel's boot code in sector 0 cannot be replaced. ``make arcart`` builds a
special OpenPsxRom for them from a dump of the cart:

.. code-block:: sh

   nops /dump 0x1F000000 0x20000 backup/ar_cart_128k.bin /dest COMx
   make arcart DUMP=backup/ar_cart_128k.bin          # -> build/openrom_arcart.rom
   python tools/psx.py flash build/openrom_arcart.rom

OpenPsxRom is linked at 1F005200h, in the writable sectors 1-4. Two small hooks
(``src/arcart_hooks.s``) sit where Datel's locked code first calls into a writable sector, at
pre-boot (1F004BCCh) and mid-boot (1F0051E0h): they install OpenPsxRom's hook and return straight
to the BIOS, after restoring what Datel's code changed on the way. ``tools/arcart_image.py`` checks
that the dump is that firmware, keeps the locked sectors byte for byte (a flash skips them as
unchanged) and Datel's settings word at 13F80h. Sector 6 holds one memory card save, sector 7 the
cheats: ``psx.py flash`` keeps both (``nops /rom`` writes the whole 128 KB, so erases them). This build
has no ``.STR`` video player (no room left in sectors 1-4). Its Cartridge screen shows the chip and
the ROM's CRC only: a 128 KB chip has no bank 2, and nothing answers at 1F040000h.

