Testing
=======

Three levels, from no hardware at all to the real console.

Level 1: PC tools and ROM image
-------------------------------

.. code-block:: sh

   python tests/test_tools.py

OMC1/LZSS round trips (empty card, random data, window limits, save-like data) and rejection of
damaged data; PS-EXE header; boot disc ISO layout; screenshot colour conversion. With
``build/openrom.rom``: size and 64 KB limit, blank data areas, Sony headers, entry points and ROM
entry table, packed payload (CRC), not mistaken for a bank 2 image.

Level 2: emulator
-----------------

.. code-block:: powershell

   powershell -ExecutionPolicy Bypass -File tests\emu_test.ps1 [-Duck DIR] [-Rom2 FILE]

Boots the ROM in DuckStation with Xplorer cartridge emulation and checks the TTY log: complete boot
log with the switch ON; with the switch OFF: bypass by default, bank 2 firmware started (with
``-Rom2``), former setting value still accepted, empty bank 2 or 3 and invalid settings refused.

Level 3: real console
---------------------

.. code-block:: sh

   python tests/hw_test.py --port COMx --nops nops.exe [--card] [--flash ROM] [--mcup CARD]

The console must be in the OpenPsxRom menu. Checks PING/PONG without echo, ``nops /ping``, the
``ONLY`` reply, the cartridge header, a 64 KB RAM round trip, ``/poke32``, a 230400 baud dump and
the return to 115200, a ``SHOT`` screenshot, and with ``--card`` a memory card read. Only RAM is
written by default; ``--flash`` and ``--mcup`` (erases the card) are opt-in.
