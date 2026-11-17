Serial protocol
===============

Compatible with ``nops`` / Unirom 8. Commands are 4 ASCII bytes, matched on a rolling window of
the last 4 bytes received (leading noise is ignored); integers are 32-bit little-endian.

Handshake
---------

For commands with data, the console answers ``OKV2`` (protocol 2 offered); the PC answers ``UPV2``
within about 150 ms to use it; then the console sends the command's reply (usually ``OKAY``).
In protocol 2, data goes in 2 KB chunks: the receiver checks each chunk's checksum and answers
``MORE`` (or ``ERR!`` to get it again).

Commands
--------

.. list-table::
   :header-rows: 1
   :widths: 14 22 34 15 15

   * - Token
     - nops
     - Then
     - Menu
     - In game
   * - ``PING``
     - ``/ping``
     - reply ``PONG``
     - yes
     - yes
   * - ``SBIN``
     - ``/bin``, ``/poke8/16/32``
     - addr, size, checksum, data
     - yes
     - yes
   * - ``SEXE``
     - ``/exe``
     - header, then data
     - yes
     - yes
   * - ``SROM``
     - ``/rom``
     - size, checksum; ``FITS`` / ``NOPE`` / ``UNKN`` / ``NONE``; data
     - yes
     - ``UNSP``
   * - ``JUMP`` / ``CALL``
     - ``/jmp`` / ``/jal``
     - address
     - yes
     - yes
   * - ``DUMP`` / ``HEXD``
     - ``/dump`` / ``/watch``
     - addr, size; data
     - yes
     - yes
   * - ``REST``
     - ``/reset``
     - jump to the BIOS
     - yes
     - yes
   * - ``DEBG``
     - ``/debug``, ``/gdb``
     - installs the debug core
     - yes
     - yes
   * - ``HALT`` / ``CONT``
     - ``/halt`` / ``/cont``
     - ``HLTD`` / ``OKAY``
     - ``ONLY``
     - yes
   * - ``HKEX`` / ``HKRD`` / ``HKWR`` / ``UNHK``
     - hook commands
     - address
     - ``ONLY``
     - yes
   * - ``MCUP``
     - ``/mcup``
     - card, size, checksum, 128 KB
     - yes
     - ``UNSP``
   * - ``MCDN``
     - ``/mcdown``
     - card; ``HLTD``, ``MCRD`` + addr + size, then a dump
     - yes
     - ``UNSP``
   * - ``BAUD``
     - ``/psxbaud N``
     - ``BPLS``, N (resent), ``BACP`` or ``NOPE``
     - yes
     - yes
   * - ``FAST`` / ``MEDI`` / ``SLOW``
     - ``/fast`` / ``/medi`` / ``/slow``
     - 518400 / 414720 / 115200 bauds
     - yes
     - yes
   * - ``SHOT``
     - ``tools/psxshot.py``
     - x, y, w, h (w = 0: the screen); reply addr, w, h
     - yes
     - no

A speed change applies to the next command only; the link then returns to 115200, where every
``nops`` session starts. ``FLAT`` and ``GAPS`` (optional EXE loading modes) answer ``UNSP``.

PCDRV
-----

While ``nops /m`` runs, a program's PCDRV ``break`` sends ``00 'p'`` + the function code (u32);
the PC answers ``OKAY`` then the exchange of that function (file name, handle, length, data).
