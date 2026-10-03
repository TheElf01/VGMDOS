# VGMDOS 2026 - TheElf

(Espanol: README_ES.md)

<img width="640" height="400" alt="vgmdos_001 raw1" src="https://github.com/user-attachments/assets/586b03f2-af32-4a16-8267-b724c1e2166f" />
<img width="640" height="400" alt="vgmdos_000 raw1 (1)" src="https://github.com/user-attachments/assets/cabc7420-077e-4cd3-8be8-354b362de8a6" />


VGMDOS is a VGM player for DOS. It runs on a 286 or better.
Songs that use FM chips play on the OPL of your sound card. The rest of the chips are emulated by software and go out through the DAC of the card.

It plays music from many systems: Master System, Game Gear, Neo Geo Pocket, Mega Drive, Mega CD, 32X, Neo Geo, SNES, NES, Famicom Disk System, PC Engine, Game Boy, WonderSwan, MSX (MSX-MUSIC, MSX-AUDIO, SCC), ZX Spectrum, Amstrad CPC, PC-88 and OPL based machines.

The "heavy" systems (Mega Drive, Neo Geo, SNES, PC Engine) are prepared first with **vgmtool**, a converter that runs on your modern PC.

## What you need

- A 286 or better. With a faster CPU you can raise the quality, but a fast 286 can already do 22 kHz stereo.

- Sound Blaster, SB Pro, SB16, AWE32/64 or compatible, with the BLASTER variable set

- Or a DAC on the printer port (Covox type): `OUTPUT=LPT1` in the INI

- PicoGUS in Tandy mode also works for the SN76489.

## Files

| File | What it is |
|------|------------|
| bin/VGMDOS.EXE | the player |
| bin/VGMDOS.INI | options |
| bin/VGMTOOL.EXE | converter for Windows (32 bits) |
| bin/VGMTOOLD.EXE | the same converter for DOS (386 or better) |

## How to use it

1. Put your VGM/VGZ files in directories on the modern PC. The best way is one directory per game.
2. Run vgmtool over the whole collection:

       vgmtool C:\VGM C:\VGMDOS

3. Copy `C:\VGMDOS` to the DOS machine and enjoy. Set `PATH=` in `VGMDOS.INI` if you want it to load another directory or drive, like a CD-ROM or the network.

You can also use VGM files without vgmtool, but Mega Drive and Neo Geo files must be converted, and files with a lot of PCM (PC Engine, some NES) are very slow on a 286 if you do not convert them.

## Keys

| Key | Action |
|-----|--------|
| Arrows, PgUp, PgDn, Home, End | move |
| Enter | open directory / play |
| Space | play the whole directory |
| S | create FILES.LST for the directory (titles taken from the VGM) |
| I | about |
| ESC | stop the song / exit |

## VGMDOS.INI

    LANGUAGE=EN        EN or ES
    PATH=C:\VGM        directory it opens at start (if not set, the one of the EXE)
    NOSHORT=1          shows the titles instead of the 8.3 names, read from VGM.DIR and FILES.LST
    QUALITY=22         frequency of the software chips: 8, 11, 16, 22 or 44
    QUALITY_NES=11     limit for one chip (PSG, AY, SCC, NES, FDS, GB, WS, PCE, MCD)
    OUTPUT=LPT1        DAC on the printer port (LPT1 to LPT4)
    PGUS_TNDY=1        PicoGUS Tandy for the SN76489 (port C0, or the port in hex)
    BITS16=1           16 bit output on SB16/AWE (needs H in BLASTER)
    PGUSPATH=C:\PGUS   directory of PGUSINIT.EXE (see PicoGUS collection)
    PGUSDRIVE=E        drive letter of the PicoGUS CD

**QUALITY** is the frequency used by the chips that are emulated by software. Higher sounds cleaner but uses more CPU. If a song is slow or jumpy, lower QUALITY, or lower only the chip that gives trouble with `QUALITY_xxx`. For example, a 286 at 12 MHz can do the PSG at 16 kHz but maybe the NES only at 11 or 8. A Harris 25 MHz can do ALL of them at 22 kHz.

**BITS16** puts the output in 16 bits for songs with PCM (Mega CD, Neo Geo, SNES). Soft parts and fades gain a lot over 8 bits. It uses a bit more CPU. Needs SB16 or better.

## VGMTOOL

    vgmtool C:\VGM C:\VGMDOS          the whole directory tree
    vgmtool input.vgz output.vgm      one file

For each file it does this:

- **Mega Drive (YM2612):** the FM goes to OPL3. The PSG stays the same. The PCM drums go as streams, ready to play in DOS. If the song also has Mega CD PCM (RF5C164), it stays the same.
- **32X:** the PWM chip (voices, drums) is converted to PCM by itself.
- **SNES (.spc files):** the SPC is emulated and each voice becomes a PCM note (8 channels), with its BRR samples converted to PCM. The length comes from the SPC tag (180 s if it has none). The SNES echo (reverb) is not played. I recommend an AWE32 or better, since it has reverb in hardware.
- **Neo Geo (YM2610):** the FM goes to OPL3 like in Mega Drive, the SSG plays as AY and the ADPCM samples are converted to PCM at full frequency. Big games (Metal Slug) need 2 to 4 MB: what fits goes to memory (conventional + XMS) and the rest is read from the file while playing. vgmtool puts first the samples that play the most, so with 512 KB of memory almost everything is in memory.
- **PC Engine and NES with PCM:** the PCM is preconverted (see below).
- **The rest** stays the same.
- And all the files are compressed.

When converting a directory it also makes 8.3 names (`01TITLE.VGM`...), a `VGM.DIR` in each directory with the real names of the subdirectories and a `FILES.LST` with the titles. This way the player shows real names and not the 8.3 ones.

### Options

    -n        do not compress
    -c        compress only, same names and the whole tree as it is
    -d        decompress only (back to normal VGM, for VGMPlay etc)
    -l        Neo Geo: PCM at half quality (almost half the size)
    -mdlow    Mega Drive: everything through OPL3
    -mdmid    Mega Drive: PCM on 2 channels, the rest OPL3 (fast on a 286, default)
    -mdhigh   Mega Drive: all the FM through PCM (386DX or better)
    -jN       (Windows NT+) N songs at the same time, for example -j4
    -en / -es language

### Mega Drive quality

The OPL3 cannot copy the YM2612 exactly, so there are three levels:

- **-mdlow:** the FM is translated to OPL3. Small files, almost no CPU, works on any 286. It sounds close, but some instruments are not the same as the console and some notes last longer than they should.
- **-mdmid:** vgmtool records every YM2612 instrument with an exact emulator (Nuked-OPN2) and also with the OPL3 (Nuked-OPL3). The instruments that the OPL3 copies worst play as recorded notes on the PCM core, but never more than 2 at the same time. Everything else stays on OPL3. This is the best balance for a 286.
  The converter makes two passes: first it looks at when each note starts and ends, and then it shares the 2 PCM channels between the sounds that need them most. A note that is already playing is never cut: if the 2 channels are busy, the new note plays on OPL3. It takes twice as long to convert.
- **-mdhigh:** every FM note is recorded and played as PCM. It is the closest to the console, but it needs a 386DX or better and the files are bigger. On a Cyrix 486DLC33 it runs perfect.

If all the instruments of a song sound close on OPL3, the song stays as -mdlow, with no PCM. 32X songs always get the PWM converted to PCM, whatever level you pick.

Converting with -mdmid or -mdhigh is slow, so use VGMTOOL.EXE on Windows and `-jN` to use all the cores of your PC, for example:

    vgmtool -mdmid -j4 C:\VGM C:\VGMDOS

With `-jN` the converted files, VGM.DIR and FILES.LST are the same as without it. It only works with VGMTOOL.EXE for Windows and when converting a directory.

### Compression

The VGM commands are compressed with LZSS (4 KB window). The header, the GD3 and the PCM banks stay uncompressed. Files end up at 25-40% of the size, and the player decompresses them on the fly while playing, it costs almost nothing even on a 286. Only VGMDOS reads this format. If you want normal files for other players, use `-d`.

### Preconvert

Some PC Engine and NES games play voices and drums by writing every PCM sample to the chip, thousands of writes per second. On a 286 just reading that is already too much. vgmtool takes out those samples, saves each piece only once in a bank inside the file and puts a "play this piece" where it started. It sounds the same and costs much less. These files only play in VGMDOS.

## PicoGUS collection

You can build a virtual collection with the CD images of a PicoGUS.

In `VGMDOS.INI`:

    PGUSPATH=C:\PGUS
    PGUSDRIVE=E

`PGUSPATH` is the directory where `PGUSINIT.EXE` is (leave it empty if it is in the PATH). `PGUSDRIVE` is the drive letter of the PicoGUS CD.

On the hard disk you make empty directories, and inside each one a `PICOGUS.DIR` file with the CD number on the first line:

    51

When you enter that directory, VGMDOS runs `PGUSINIT.EXE /cdload 51`, waits for the CD to be ready and shows what is on the CD drive, with its `VGM.DIR` and `FILES.LST` if it has them. With ".." you go back to the hard disk directory.

If that CD is already loaded (same number), it is not loaded again. VGMDOS only remembers the last number while it is open.

The second line is optional. It is a directory inside the CD, shown instead of the root. This way one CD image can hold many collections:

    51
    MD

## AWE mode (experimental, it does not sound good yet; I would finish it if someone is interested)

For AWE32/AWE64 cards with RAM, you can let the EMU8000 chip of the card play the PCM of the converted Mega Drive songs, so the CPU works less.

    SET AWE32=1          (uses 512 KB of the RAM of the card)
    SET AWE32=2048       (or the KB you want to use)

It needs `E=` in the BLASTER variable (for example `E620`), and it only works with songs converted with this vgmtool.

This mode is not finished. CPU use goes down a lot, but the sound is not as good as the software PCM, so it is off unless you set `AWE32`.

## Building

You need Open Watcom 1.9 or 2.0.

- Player: go to the `vgmdos` directory and run `build.bat`. It makes `build\vgmdos.exe`. Python is used to turn `FONT.F16` into `vgafont_data.inc`.
- Converter for Windows: go to the `vgmtool` directory and run `build_win.bat`.
- Converter for DOS: go to the `vgmtool` directory and run `build_dos.bat`.

## Credits

vgmtool uses Nuked OPL3 and Nuked OPN2 by Nuke.YKT, and snes_spc by Shay Green. See `THIRD_PARTY.md`.

Mega Drive/Neo Geo on OPL3 is a conversion, not emulation: it sounds very close but not exactly like the original.
