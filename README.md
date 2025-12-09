# UAE4ALL for SF2000 / GB300

Amiga 500 emulator for Data Frog SF2000 and GB300 handheld devices.

Based on uae4all-rpi by Chips-fr, ported to SF2000/GB300 multicore framework.

## Downloads

| Platform | File | Size |
|----------|------|------|
| SF2000 | `releases/uae4all058_sf2000.zip` | ~495 KB |
| GB300 | `releases/uae4all058_gb300.zip` | ~495 KB |

## Installation

### SF2000

1. Extract `core_87000000` from the SF2000 zip
2. Copy to SD card: `cores/amiga/core_87000000`
3. Copy Kickstart ROM to: `bios/kick13.rom` (or kick20.rom, kick30.rom)
4. Copy ADF games to: `ROMS/` folder
5. Create ROM stubs: `amiga;GameName.adf.gba` (empty files)

### GB300

1. Extract `core_87000000` from the GB300 zip
2. Copy to SD card: `cores/amiga/core_87000000`
3. Copy Kickstart ROM to: `bios/kick13.rom` (or kick20.rom, kick30.rom)
4. Copy ADF games to: `ROMS/` folder
5. Create ROM stubs: `amiga;GameName.adf.gba` (empty files)

## Kickstart ROMs

Place Kickstart ROMs in `bios/` folder with these filenames:

| Filename | Version |
|----------|---------|
| `kick13.rom` | Kickstart 1.3 |
| `kick20.rom` | Kickstart 2.0 |
| `kick31.rom` | Kickstart 3.1 |

Kickstart 1.3 recommended for best compatibility.

## Controls

| Button | Action |
|--------|--------|
| D-Pad | Joystick directions |
| A | Fire button 1 (Red) |
| B | Fire button 2 (Blue) |
| L | Previous disk |
| R | Next disk |
| SELECT | Toggle virtual keyboard |
| START | Open menu |

## Multi-Disk Games

For multi-disk games, name your ADF files with disk number:
- `Game (Disk 1 of 3).adf`
- `Game (Disk 2 of 3).adf`
- `Game (Disk 3 of 3).adf`

Use L/R buttons to swap disks during gameplay.

## Build Differences: SF2000 vs GB300

Both platforms use the **same libretro core** (`.a` library file). The core is compiled with identical flags:

```
platform=sf2000
MIPS toolchain: /tmp/mips32-mti-elf/2019.09-03-2/
Flags: -EL -G0 -mips32 -msoft-float -DSF2000
```

The **only difference** is the final linking step, which uses platform-specific:
- **Linker script** (`bisrv_08_03.ld`) - contains firmware memory addresses
- **Firmware binary** (`bisrv_08_03.asd`) - device-specific firmware

| Component | SF2000 | GB300 |
|-----------|--------|-------|
| Core `.a` file | Identical | Identical |
| Linker addresses | SF2000 firmware | GB300 firmware |
| Final `core_87000000` | ~1.67 MB | ~1.67 MB |

### Build Process

1. Compile libretro core → `uae4all_libretro_sf2000.a` (same for both)
2. Link with platform framework:
   - SF2000: `sf2000_multicore` + SF2000 linker script
   - GB300: `gb300_multicore` + GB300 linker script
3. Output: `core_87000000` (platform-specific binary)

## Known Issues

- Some AGA games may not work (this is an OCS/ECS emulator)
- CPU speed auto-fix activates after 3 seconds on first load
- Virtual keyboard layout is basic

## Credits

- Original uae4all by Chui, john4p, TomB, notaz
- Raspberry Pi port by Chips-fr
- SF2000/GB300 port for multicore framework
