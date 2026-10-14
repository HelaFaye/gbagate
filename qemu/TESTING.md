# Testing the input modes and the app modes

Two different jobs, and it is worth being clear which is which.

**App modes** are testable automatically: the tester ROM drives the gate
protocol and reports a verdict, so a script can walk the configurations and
print a table.

**Input modes are not.** The ROM cannot see them -- it talks to the link port,
not the buttons. What a matrix run proves about an input mode is that the
firmware boots, links and reaches the suite with that setting stored, which
matters because the setting is read at startup. Whether the buttons *feel*
right is a human judgement, and the commands for making it are below.

---

## Once

```
./qemu/setup.sh
make -C rom                      # the tester ROM
./qemu/build.sh gate --aux-uart 0
./qemu/build.sh link --aux-uart 0
./qemu/build.sh full --aux-uart 0
```

`--aux-uart 0` only if your QEMU wires two UARTs rather than three; run
`./qemu/uart2check.sh` if the screen stays black.

## Everything, automatically

```
make                             # 24 host suites, no hardware
make lint                        # 19 compile and structural checks
./qemu/matrix.sh                 # every variant x input mode, with a table
```

Narrower runs:

```
./qemu/matrix.sh --quick                 # gate image, all four input modes
./qemu/matrix.sh --variants "gate full"
./qemu/matrix.sh --modes "0 3"           # encoder and 5-way+5 only
./qemu/matrix.sh --sim                   # no QEMU; gate_sim stands in
```

Modes are `0` encoder, `1` 5-way+2, `2` 5-way+4, `3` 5-way+5. Per-row logs land
in `build/matrix/<variant>-<mode>/`.

## By hand, which is the only way to judge the feel

```
./qemu/lab.sh --aux-uart 0
```

The viewer's keys drive a **virtual switch bank**, not a shortcut: they set
bits that `ui_input.c` debounces exactly as it does real pins, so the mode
under test is the real one.

| key | switch |
|---|---|
| `u` `d` `l` `r` | the five-way's directions |
| `s` or Enter | A |
| `S` | A, held past the long-press threshold |
| `b` | B |
| `f` | Select |
| `[` `]` | L and R (four- and five-switch modes) |
| `m` / `M` | Start, tapped / held (five-switch mode) |

To change input mode: **Settings -> input**, then Enter to edit, `u`/`d` to
change, Enter again. It is stored, so it survives a reboot.

### What to check in each mode

- **encoder** -- `S` (hold) is the only way back. If it does not work you are
  stuck in the mode, which is the one failure that must not happen.
- **5-way + 2** -- `b` goes back, `f` marks a favourite, `S` opens sort. `[`
  and `]` must do **nothing**: those switches are not fitted.
- **5-way + 4** -- as above, and `[` `]` page the list.
- **5-way + 5** -- `m` opens sort, and A now responds on the press rather than
  the release. That difference is the reason for the fifth switch and is the
  thing to feel for.

The footer names the controls for whichever mode is selected, so if it says
`hold A sort` you are on a two- or four-switch board, and `Start sort` means
five. A footer that disagrees with the Settings screen is a bug.

## Every app mode

From the main menu. Which modes appear depends on what the image supports and
what was detected at boot, so the `full` image shows the most.

| mode | how to tell it works |
|---|---|
| Battle Chip | chip list, tabs, uses column; `f` stars a chip |
| Send Program | lists the FAT volume, or says why it cannot |
| Read Chip | slot occupancy; reports no reader under emulation |
| Link: USB / Wireless | bridge state, words sent and received, RTT |
| USB Storage | says who owns the volume; Enter hands it over |
| Infrared | says no transmitter under emulation |
| Self Test | **run this one**: it sends its own chips, so the ROM's six checks all pass |
| Settings | six editable fields, including input |
| Diagnostics | detected capabilities, link state, counters |

Self Test is the one worth running deliberately: it is the only mode that makes
the tester ROM's chip checks pass, because it sends chips without anyone
pressing a button.

```
cat build/lab/rom_test.result
```

`tests=3F` with all six lines `pass` is a complete run.
