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
       and the ID probe at each boot wiped it). Write bank 3 with ``make flash3 IMG=...``
       (``build/flash3.exe``, run with ``nops /exe``), which writes and reads back 128 KB.
   * - Xplorer FX, single 512 KB chip
     - Winbond W29C040 (reported on forums)
     - Not tested. Unknown chips are reported with their ID in the boot log.
   * - Datel (Action Replay / PS Hacker)
     - AMD Am29F010 (128 KB)
     - One bank. Some sectors may be hardware-protected (12 V needed to unprotect).

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
2 (or 3). Unirom's own installer always writes bank 1: use ``nops /rom`` with OpenPsxRom, which
detects the target bank (see :doc:`installing`).

Serial cable
------------

SIO1, 115200 bauds 8N1, 3.3 V levels. PS1 serial port pins: 1 RXD (to the adapter's TX), 4 TXD
(to the adapter's RX), 5 CTS (the PS1 only transmits when the PC raises RTS), 7 GND. Pin 2 is
+3.5 V: do not connect it to the adapter.
