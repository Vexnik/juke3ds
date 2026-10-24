# Juke3DS

A music player homebrew app for the Nintendo 3DS family (3DS, 3DS XL, 2DS, New 3DS, New 3DS XL, New 2DS XL).

Juke3DS aims to play every audio format the 3DS family can handle. Right now it supports MP3 and WAV, and more formats are planned (see the roadmap).

## Features

- Plays MP3 and 16-bit PCM WAV files
- Folder browsing on the SD card, including subfolders
- Plays through a folder automatically, with shuffle and repeat (off / all / one)
- Skip forward and back 10 seconds
- Shows title, artist, a progress bar, and elapsed / total time (from MP3 tags where available)
- Tries to keep playing with the lid closed (experimental, see Notes)

## Requirements

- A 3DS with custom firmware (for example Luma3DS) and the Homebrew Launcher
- The DSP firmware file at `sdmc:/3ds/dspfirm.cdc`. If Juke3DS says it can't start audio, run the "DSP1" homebrew app once to create it.

## Installing

1. Download `juke3ds.3dsx` (see "Building" below if you don't have one yet).
2. Copy it into the `3ds` folder on your SD card.
3. Create a folder named `music` at the root of your SD card and put your audio files in it. Subfolders are fine.
4. Launch Juke3DS from the Homebrew Launcher.

## Controls

| Button | Action |
| --- | --- |
| Up / Down | Move through the list |
| A | Open a folder, play a song, or pause / resume the song that is already loaded |
| B | Go up one folder |
| X | Pause / resume |
| Left / Right | Skip back / forward 10 seconds |
| L | Restart the song (if more than 3 seconds in), otherwise previous song |
| R | Next song |
| Y | Shuffle on / off |
| SELECT | Repeat: off / all / one |
| START | Exit |

Volume is controlled with the 3DS volume wheel.

## Building

### With GitHub Actions (no setup needed)

Every push to this repository builds the app automatically. Open the Actions tab, select the latest run, and download the `juke3ds` artifact at the bottom of the page. It contains `juke3ds.3dsx`.

### On your own computer

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS toolchain.
2. Install the MP3 library: `dkp-pacman -S 3ds-mpg123`
3. Run `make` in the repository folder.

## Notes

- Lid-closed playback is experimental. Test it with headphones plugged in, as the speakers are switched off when the lid is closed.
- The song length of some MP3 files (mainly variable bitrate files without a length header) is an estimate, so the progress bar can end slightly early or late.
- Lists are limited to 256 entries per folder for now.
- Older 3DS models have a slower processor, so very high-bitrate files may stutter there.

## Roadmap

- [x] MP3 and WAV playback
- [x] Folders, shuffle, repeat, seeking, song info
- [ ] More audio formats (OGG Vorbis, Opus, FLAC, AAC / M4A)
- [ ] Touchscreen controls
- [ ] Graphical interface
- [ ] Remember last folder and settings between launches

## Credits

- [devkitPro](https://devkitpro.org/) and [libctru](https://github.com/devkitPro/libctru) for the 3DS toolchain and libraries
- [mpg123](https://www.mpg123.de/) for MP3 decoding
