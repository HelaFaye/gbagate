# Save Tool: a cartridge's save, to the card and back

**Applets → Save Tool** backs a GBA cartridge's save up to the board's
microSD card, and writes a `.sav` from the card back to the cartridge. In
between, the `.sav` is an ordinary file: edit it with
[PKHeX](https://github.com/kwsch/PKHeX) on a computer, with the card in a
reader or through the board's USB drive, then restore it.

It is an add-on: the menu lists it only when its GBA half is on the card.

## Setting it up

1. Build the GBA half: `make -C rom savetool` (the bare ARM toolchain, as for
   the agent), giving `rom/savetool.bin`.
2. Put it on the card as `savetool/savetool.bin`.
3. The board lists **Save Tool** at the next start. It logs
   `Save Tool: on the card, in the menu`, or `not on the card`.

The board sends the memory agent first (doc/AGENT.md), so it borrows the boot
logo from a program on its drive as Send Program does.

## Using it

1. Put the cartridge in the GBA. Hold **START + SELECT** while turning it on,
   and link it to the board. Holding them makes the BIOS skip the cartridge
   and wait for a multiboot program, with the cartridge still in. *To be
   confirmed on a real GBA:* every test so far is in emulation.
2. **Start.** The board sends the agent, then the Save Tool's GBA half. It
   finds the save library the game names and reads the flash chip's ID. The
   page shows the game code, title and save type.
3. **Back up to card** writes `saves/<game code>-<n>.sav`, for example
   `saves/BPEE-001.sav`, numbered on from the last.
4. **Restore from card** lists the `.sav` files in `saves/` that are exactly
   this save's size.
   - Pick one and confirm.
   - The save on the cartridge is always backed up first, as the next
     number.
   - Then the file is written 4 KB at a time. Each sector is read back on the
     GBA and compared, and a sector that does not match stops the restore and
     says which.

Keep the GBA on while it writes. A restore that stops part-way leaves a save
that is part old and part new. The backup made just before it is on the card,
so restore that.

## What it supports

| save | how it is found | read | write |
|---|---|---|---|
| SRAM, 32 KB | `SRAM_V`, `SRAM_F_V` in the ROM | yes | yes |
| flash, 64 KB | `FLASH_V`, `FLASH512_V` | yes | yes (Atmel by 128-byte pages, the others by sector erase and byte program) |
| flash, 128 KB | `FLASH1M_V` | yes, both banks | yes |
| EEPROM | `EEPROM_V` | not yet: it says so | not yet |

**The chip decides the size, not the string.** Some games name `FLASH512` on
a 128 KB part, or the reverse. When the chip's ID is one of the parts GBA
cartridges carry, it sets the size:
- 128 KB: Sanyo LE26FV10N1TS (`0x1362`) and Macronix MX29L010 (`0x09C2`);
- 64 KB: Atmel AT29LV512, Panasonic MN63F805MNP, SST SST39LVF512 and
  Macronix MX29L512.

Every Generation 3 Pokémon game uses 128 KB flash. EEPROM saves (a serial
protocol through DMA, at `0x0D000000`) are left for later.

## How it works

The GBA runs the memory agent and this blob, nothing else.
- **The blob** is `rom/savetool.c`, linked at `0x02001000`. It runs from
  EWRAM, as code that sends flash commands must, and its save buffer is
  `0x02010000`.
- **The control block** is the blob's first 60 bytes (`rom/savetool.h`). The
  board writes a command there and runs the blob with the agent's CALL, then
  reads the block back.
- **The commands:** DETECT scans 4 MB of ROM a call; FLASH_ID; READ fetches
  the whole save into the buffer; WRITE does one 4 KB sector, verified. No
  call runs long.
- **On the board,** the applet (`applets/save_tool.c`) streams the save
  between the GBA's buffer and the card 4 KB at a time, so it needs no
  128 KB buffer. It does one step a tick, so the page shows progress.
- **File services:** applets get them in `applet_env_t` (`file_size`,
  `file_read`, `file_write`, `file_list`, over `/sd`), and an add-on names its
  file in `applet_t.needs_file`.

Prior art: jer-irl/gba-savedumper (GPL-3.0) dumps saves on the GBA by
swapping cartridges. On real hardware, putting a flash cart back in reset the
GBA. Multibooting from the board avoids swapping altogether: the game
cartridge stays in.

## Tested

- `test_save_tool` (host, in `make`) runs the real applet, the real agent
  client and the agent's protocol core, and the real blob, compiled for the
  host, on a modelled cartridge with SRAM and a temporary card:
  - the applet is listed only with its file on the card;
  - detection, and the game code and title;
  - numbered backups, byte for byte;
  - the restore list (only `.sav` files of the right size);
  - a restore of an edited save, and the save it replaced kept first;
  - refusals: a file that is not the blob, a game with no save library.
- `test/mgba/savetool_emerald.py` (in `make mgba-emerald`; it needs the ROM)
  runs the ARM blob on a real Emerald cartridge's 128 KB flash, as mGBA
  emulates the chip:
  - DETECT finds `FLASH1M_V`, and the chip reads Sanyo `0x1362`;
  - a 64 KB string on this chip is read as 128 KB;
  - READ brings both banks byte for byte;
  - WRITE puts a new save in all 32 sectors, each verified on the GBA, and
    mGBA's own copy of the flash then matches it exactly.
- On qemu_esp32, the touch carrier's firmware lists the applet with
  `savetool/savetool.bin` on its card and not without it
  (`GBAGATE_CARD_ADD` in `qemu/accurate_boot.py`).

Not yet tested:
- a real GBA and cartridge;
- the agent and blob together over a real link (the applet's tests use the
  protocol core, not the multiboot transfer);
- SRAM and the Atmel, Macronix and Panasonic flash parts in mGBA, which models
  only Sanyo for 128 KB.

## With PKHeX

PKHeX opens a `.sav` as it is: it needs no plugin to edit what this tool
backs up. A PKHeX plugin that talks to the board directly (back up, open,
save, restore without moving files) is the next step, in
HelaFaye/PKHeX-GBA-Multiboot.
