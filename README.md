# SF2000-UAE v073 - Amiga 500 Emulator

Amiga 500 (OCS) emulator for Data Frog SF2000 and GB300 handheld devices.

Based on uae4all-rpi by Chips-fr.

## ⚠️ Important

- **Only Kickstart 1.3 and 2.0 work** - Kickstart 3.0/3.1 does NOT boot
- **OCS chipset only** - ECS and AGA games will not run correctly
- Emulates classic Amiga 500 with 68000 CPU

## Memory

| Type | Amount |
|------|--------|
| Chip RAM | 2 MB (fixed) |
| Slow RAM | Off / 512KB / 1MB / 1.5MB |

## Controls

| Button | Joystick | Mouse Mode |
|--------|----------|------------|
| D-Pad | Directions | Mouse move |
| B | Fire | Left Click |
| A | Fire | Right Click |
| L | - | Left Click |
| R | - | Right Click |
| SELECT | Virtual Keyboard | |
| START | Menu | |

## Kickstart ROMs

Place in `bios/` folder:

| File | Version |
|------|---------|
| `kick13.rom` | 1.3 (34.005) |
| `kick20.rom` | 2.0 (37.175) |

⚠️ Kickstart 3.x does NOT work with this emulator.

## Installation

1. Copy `core_87000000` to `cores/amiga/`
2. Copy `kick13.rom` to `bios/`
3. Copy `.adf` games to `ROMS/`
4. Create empty stub: `amiga;GameName.adf.gba`

## Changelog

### v073
- Slow RAM: Off/512KB/1MB/1.5MB
- Mouse: B=LMB, A=RMB
- Scroll arrow indicator
- Removed broken Fast RAM

### v070-v072
- Mouse Speed 1-8
- Delete Config
- Settings scroll

## Credits

- uae4all by Chui, john4p, TomB, notaz
- Raspberry Pi port by Chips-fr
- SF2000 multicore framework by kobily, madcock
- SF2000/GB300 port by Grzegorz Korycki
- Everybody at Retro Handhelds Discord
