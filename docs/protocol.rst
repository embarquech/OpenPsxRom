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

OPX1 extended link (``tools/psx.py``)
-------------------------------------

OpenPsxRom's own client, next to ``nops`` (everything above is unchanged: ``nops`` never sends
``OPX1``). The PC sends the token ``OPX1`` at 115200 baud; the console answers ``OPXA`` and both
then exchange *frames*, until ``END``, a flash, an EXE start, a reset or about 3 s of silence:

.. code-block:: none

   A5h, cmd (u8), len (u16), payload[len], CRC32 (u32, zlib) of cmd..payload     little-endian

Each request gets exactly one answer frame with the same ``cmd``; its payload is a status byte
(0 ok, 1 damaged frame, 2 bad arguments, 3 refused, 4 failed, 5 unknown command) followed by the
data. The PC sends the next request only once it has the answer. A frame damaged on the way to the
console is answered with ``cmd`` FFh, status 1; one damaged on the way back fails its CRC: in both
cases the PC simply sends the request again (every request can be repeated safely). A PC that lost
the session can send ``OPX1`` again inside it: the console answers ``OPXA`` and goes on.

.. list-table::
   :header-rows: 1
   :widths: 6 10 42 42

   * - cmd
     - Name
     - Request payload
     - Answer data
   * - 0
     - ``PING``
     -
     -
   * - 1
     - ``INFO``
     -
     - text, one ``key=value`` per line: version, build (bank1 / bank3 / arcart), flash chip and
       ID, size, banks 2 and 3 available, ``rombuf`` (ROM buffer, 80100000h), ``romram`` (RAM used
       by OpenPsxRom, never writable), ``openrom_size``, ``max`` (largest data payload), ``div``
   * - 2
     - ``BAUD``
     - divisor (u32) of 2073600, as ``/psxbaud``
     - answered at the old speed, then the port is reset at the new one; no valid frame within
       ~1 s: back to 115200
   * - 3
     - ``READ``
     - address, length (up to 4 KB)
     - the bytes (any address: RAM, flash, scratchpad)
   * - 4
     - ``WRITE``
     - address, data
     - main RAM or scratchpad only, never OpenPsxRom's own RAM
   * - 5
     - ``LZWR``
     - address, OMC1 stream (:doc:`formats`, ``tools/omc.py``)
     - unpacked at the address, CRC of the unpacked data checked
   * - 6
     - ``CRCS``
     - address, step, count (up to 512)
     - ``count`` CRC32s of ``step`` bytes each
   * - 7
     - ``COPY``
     - destination, source, length
     - e.g. a flash bank to ``rombuf``
   * - 8
     - ``FLASH``
     - CRC32 of the 128 KB in ``rombuf``
     - written as ``SROM`` would (bank from the image's entry, OpenPsxRom's saves kept); answer:
       failed blocks (u32), bank (u32), or the reason of a refusal. The session ends on the flash
       screen
   * - 9
     - ``EXEC``
     - the 2048-byte PS-X EXE header (data loaded before)
     - answered, then the EXE starts
   * - 10
     - ``RESET``
     -
     - answered, then jump to the BIOS reset vector
   * - 11
     - ``PROG``
     - done, total (u32), then title and up to 4 lines, NUL-separated
     - the console draws that progress screen (the PC drives the bar)
   * - 12
     - ``END``
     -
     - session closed, back to the menu's serial poll
   * - 13
     - ``FLAREA``
     - offset, length, CRC32 of that much of ``rombuf``
     - writes a data area of bank 1 (whole 16 KB sectors, never OpenPsxRom's code; refused if the
       cartridge does not hold OpenPsxRom), then reloads the cheat list

``psx.py`` uses them as follows:

* ``flash FILE`` (*differential flash*): ``CRCS`` of the cartridge bank in 1 KB pieces, compared
  with the file; ``COPY`` of the bank to ``rombuf``; only the pieces that differ are sent
  (``LZWR`` when compressing makes them smaller, else ``WRITE``); ``CRCS`` of ``rombuf`` against
  the file; then ``FLASH``. An image already on the cartridge is not written again. ``--full``
  sends every piece.
* ``exe FILE`` / ``load FILE ADDR``: the data in compressed 1 KB pieces, then ``EXEC`` for an EXE.
* ``dump ADDR LEN FILE`` / ``peek ADDR [LEN]``: ``READ`` in 1 KB pieces.
* ``info``, ``ping``, ``reset``.
* ``tools/cheats.py`` reads the cheat sector with ``READ`` and writes it back with ``FLAREA``
  (see :ref:`cheat-database`).

.. code-block:: sh

   python tools/psx.py flash build/openrom.rom          # port from tools/comport.txt, or -p COMx
   python tools/psx.py dump 0x1F000000 0x20000 bank1.bin

The link stays at 115200 baud. The PS1 cannot hit the PC's rates exactly (33.8688 MHz / 16 / 18 =
117600 baud, 2 % fast), and with a fixed-rate adapter (CP210x) some bytes arrive wrong, mostly in
data with many 0/1 transitions: the 1 KB pieces and their resend keep that cheap (about 9 KB/s).
``--fast`` asks for divisor 4 (PC at 510000, as ``nops /fast``); it only helps with an adapter that
can follow the console's real rates (FTDI, CP2102N). Set ``PSX_DEBUG=1`` to see every resend.
