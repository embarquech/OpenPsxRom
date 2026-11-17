Status
======

Tested on real hardware (Xplorer, SST 29EE020)
----------------------------------------------

* boot, boot log, menu over serial;
* ``nops``: ``/ping``, ``/bin``, ``/exe``, ``/rom``, ``/dump``, ``/poke``, ``/mcup``, ``/mcdown``,
  ``/debug``, 230400 bauds;
* flashing with read-back verification; switch and bank 2 (Unirom); second chip detection on a
  double-chip Xplorer FX;
* memory card writes with frame verification; screenshots over serial;
* printf to serial and register reading under the debug core.

Not tested on hardware yet
--------------------------

In-game menu, cheats, code finder, memory card manager and icons, CD audio player, STR player,
PCDRV, GDB sessions and hardware breakpoints, writing bank 3.

Known limitations
-----------------

* Running other software that writes to the expansion port can leave the Xplorer's logic in a
  state that only a power cycle clears.
* The 1F060000h-1F06FFFFh area of double-chip carts is hidden by the Xplorer registers.
* ``/fast`` (518400 bauds) needs a USB adapter able to do it; ``/psxbaud 230400 /pcbaud 230400``
  works with common CP210x adapters.
* Sector protection of AMD chips (some Datel carts) can only be removed with 12 V on A9 and OE#.
