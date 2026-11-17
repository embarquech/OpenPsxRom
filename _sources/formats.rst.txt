Formats
=======

Flash layout (bank 1)
---------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Content
   * - 00000h
     - 64 KB
     - OpenPsxRom: header, ROM-executed code, OMC1-packed RAM payload
   * - 10000h
     - 48 KB
     - memory card saves: OFS1 container packed with OMC1
   * - 1C000h
     - 16 KB
     - cheat list ``OCH1``; the last 128 bytes (1FF80h) are the settings page ``OCF1``
   * - 20000h
     - 128 KB
     - bank 2 (optional second firmware)

Cartridge header
----------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Content
   * - 00h
     - mid-boot entry (address)
   * - 04h
     - "Licensed by Sony Computer Entertainment Inc."
   * - 30h
     - "OpenPsxRom - open source cartridge firmware" (also used to recognise OpenPsxRom images)
   * - 80h
     - pre-boot entry (address), called by the BIOS
   * - 84h
     - "Licensed by Sony Computer Entertainment Inc." (the BIOS checks it)
   * - ECh-FCh
     - entries of the ROM-executed code: rom_break, rom_feed, rom_halted, rom_crash, rom_menu

OMC1 (compressed data)
----------------------

Used for the ROM payload and the save store. ``'OMC1'``, u32 raw size, u32 CRC32 of the raw data,
u32 packed size, then LZSS: a flag byte (bit 0 first; 1 = literal byte, 0 = match), a match being
2 bytes ``b0 = (dist-1) & 255``, ``b1 = ((dist-1) >> 8) << 4 | (len-3)``; distance 1-4096, length
3-18. The decoder rejects a match pointing before the start of the output and checks the CRC.
``tools/omc.py`` packs, unpacks and exports saves.

OFS1 (save store)
-----------------

``'OFS1'``, u32 save count, then per save its 128-byte directory frame (size at 4-7, name at 10-30)
followed by its data rounded up to 8 KB blocks. The whole container is packed with OMC1 into the
48 KB area. ``python tools/omc.py saves store.bin`` writes one ``.mcs`` per save from a dump of
1F010000h.

OCH1 (cheats)
-------------

``'OCH1'``, u32 count, then per code: u32 address, u32 value | enabled << 16 (96 codes max).

OCF1 (settings)
---------------

At 1FF80h: ``'OCF1'``, u32 flags. Bits 0-7: what the switch OFF starts at power-on: 0 bypass, 2 bank
2 (1 = the same, older setting), 3 bank 3. Read by the pre-boot code before anything else; saving
the cheats keeps this page.
