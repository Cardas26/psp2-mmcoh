# Might & Magic: Clash of Heroes for PS Vita

Wanted to play this game on the Vita myself, so vibecoded it.

## What works

- All main gameplay functionality, including FMOD Designer based Music/SFX,
  which was a challenge to get working.
- Single-player campaign. Tested up to the end of the Haven campaign.
- Touch everywhere; buttons on most screens (see *Controls*).
- Not available: PS TV support and Online features / Battle Mode (discontinued
  by Ubisoft)

## Requirements

- Custom firmware enabled PS Vita
- Install the following modules:
  - [kubridge](https://github.com/bythos14/kubridge/releases)
  - [libshacccg](https://cimmerian.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx)
  - [FdFix](https://github.com/TheOfficialFloW/FdFix/releases/). Don't install
    this if RePatch is already installed.
  - Optional: Install [PSVshell](https://github.com/Electry/PSVshell) to
    increase CPU and GPU clock speeds
- 1 GB free space on `ux0:` for setup, 600 MB afterwards.
- The Android game, **version 1.4 only**:
  - .apk file
  - .obb file

## Automatic setup

1. Install `mmcoh.vpk` from the [latest release](../../releases/latest) 
2. Create the `ux0:data/mmcoh/` folder
3. Copy into it:
   - the .apk file, renamed to `ClashOfHeroes.apk`;
   - the .obb file with its default name
4. Launch the game, answer **Yes** to *Set up now?*.
   - Setup unpacks, checks and patches the files, then deletes the .obb file.
   - Keep the Vita on; it takes a few minutes. The game starts when done.
5. Keep `ClashOfHeroes.apk`: the game reads it at every launch.

Note: if automatic setup fails, manual setup below can also be used.

### Manual setup

Needs [Python 3](https://www.python.org/downloads/) and
[`prepare_data.py`](tools/prepare_data.py).

1. Build the folder:
   ```
   python3 prepare_data.py <the .apk> <the .obb> mmcoh
   ```
2. Copy the **contents** of `mmcoh` to `ux0:data/mmcoh/`
3. Eject, reconnect, and check the copy:
   ```
   python3 prepare_data.py --verify <card>/data/mmcoh
   ```
4. Copy again any file it reports, and eject.
5. Launch the game. Automatic setup does not run.

### Updating

- Install the new VPK over the old one. Saves are kept.

## Controls

Touch works everywhere. Physical button controls cover the map, battles and
most menus (see below).

The defaults are below; remap any of them in `ux0:/data/mmcoh/controls.ini`.

| Where | Button | Action |
|---|---|---|
| Map | D-pad or Left stick | move the hero |
| | Cross | interact with the hero's tile |
| | L / R | quest log / pause menu |
| Battle | D-pad or Left stick | move along columns and rows. Hold to speed up  |
| | Cross / Circle | pick up or drop a unit / cancel |
| | Triangle | remove the selected unit |
| | Square | cast the hero's spell |
| | SELECT | call reinforcements |
| | START | end the turn |
| | R | zoom |
| Conversations and pop-ups | Cross / Circle | continue, yes / no, close |
| | D-pad | choose an answer |
| Pre-battle screen | Cross / Circle / Square | battle / flee / arrange units |
| Other screens | D-pad | move the on-screen cursor |
| | R | tap under the cursor (hold L to move it slowly) |

## Known issues

- Full physical button support was not implemented by TAG Games, and will
  need more work to implement
- Busy battles can briefly drop fps below 60. Set CPU to 500MHz and GPU to
  222MHz to reduce these frame dips (defaults: 444 MHz CPU and 166MHz GPU).
- A screen's first visit can pause while it loads.
- The ENEMY TURN banner flashes black, and battles have fewer effects than
  on PC. The stock Android game does the same.
- PS TV: no touch, so screens without button controls are unusable.

## Reporting a bug

1. Install the **diagnostics** build, `mmcoh-diagnostics.vpk` from the [latest release](../../releases/latest).
2. Reproduce the problem.
3. Attach to a [bug report](../../issues/new/choose):
   - `ux0:data/mmcoh/debug.log`;
   - `ux0:data/mmcoh/watchdog.log`, if present;
   - `ux0:data/mmcoh/Cache/MoFloLog.txt`;
   - after a crash, the newest `ux0:data/*.psp2dmp`.

## Building from source

Needs [Podman](https://podman.io/) or Docker (replace `podman` with `docker`).

```
podman build -t psp2-mmcoh .
podman run --rm -v "$PWD":/src:Z -w /src/loader psp2-mmcoh \
  sh -c "cmake -B build && cmake --build build"
```

- Output: `loader/build/mmcoh.vpk`.
- Diagnostics build (`debug.log`, hang watchdog, frame-rate log):

```
podman run --rm -v "$PWD":/src:Z -w /src/loader psp2-mmcoh \
  sh -c "cmake -B build-diag -DDIAGNOSTICS=ON && cmake --build build-diag"
```

## Credits

- Built with [Claude Code](https://claude.com/claude-code): the reverse
  engineering, the loader, the fixes and the many performance patches.
- [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate)
  and [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) by Volodymyr
  Atamanenko.
- [so_util](https://github.com/Rinnegatamante/so_util) by Andy Nguyen and
  Rinnegatamante.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) and
  [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) by Rinnegatamante.
- [kubridge](https://github.com/bythos14/kubridge) and
  [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) by bythos.
- [VitaSDK](https://vitasdk.org/).
- Full list and licences: [THIRD_PARTY.md](THIRD_PARTY.md).

## Legal

- No game code or data is included or linked.
- No donations are accepted.
- The port's code is MIT licensed ([LICENSE](LICENSE)). Vendored libraries
  keep their own licences; vitaGL is LGPLv3, so every release links the
  source it was built from ([THIRD_PARTY.md](THIRD_PARTY.md)).
