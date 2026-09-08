# Scripts

Everything here runs through the `do` wrapper, which finds the ESP-IDF
virtualenv (the python that has pyserial) and the IDF itself, and then gets out
of the way. It has to be run by path — `do` is a keyword in both shells:

```powershell
.\do.ps1 <script> [arguments]     # PowerShell
./do <script> [arguments]         # bash
.\do.ps1                          # what there is to run
```

Anything after the script name is passed through, so `.\do.ps1 upload --help`
still reaches the script's own help. `restore-flash` exists as both a `.sh` and
a `.ps1` and each wrapper picks its own; everything else is one file, and `./do`
hands a `.ps1` to `powershell.exe` rather than refusing it. The examples below
are written for PowerShell because most of these are `.ps1`, which is where the
`-Switch` spelling comes from.

No script needs a `--port`: the tablet is found by the ESP32-P4's USB VID.
Machine-local paths and overrides live in `.env.local` — see
[.env.local.example](../.env.local.example).

## Setup

| | |
|---|---|
| `setup-toolchain` | Find, install and record the ESP-IDF the firmware builds against. |

```powershell
.\do.ps1 setup-toolchain -Check          # what is installed; changes nothing
.\do.ps1 setup-toolchain                 # ... then install v5.4.2 if needed
.\do.ps1 setup-toolchain -Yes            # no prompt before the download
.\do.ps1 setup-toolchain -IdfPath D:\esp\v5.4.2\esp-idf
.\do.ps1 setup-toolchain -Reinstall      # repair a half-finished toolchain
.\do.ps1 setup-toolchain -Reinstall -Targets esp32p4,esp32c6
```

`-Check` first: "no ESP-IDF found" has three causes — none installed, the wrong
version installed, or the right one in a layout the scripts do not look in — and
only the first needs a download. It lists every checkout it can find with its
version, says which one the scripts would pick, and writes `IDF_PATH` /
`IDF_TOOLS_PATH` into `.env.local` (keeping whatever else is in there).

The pin is v5.4.2 and not a floor: `neos/main/idf_component.yml` overrides the
`espressif/usb` dependency precisely because the BSP's own choice will not
compile against it.

## Build

| | |
|---|---|
| `build-all` | Firmware and every app, then `abi_check`. |
| `abi_check` | Check built app ELFs against the NeOS ABI. |

```powershell
.\do.ps1 build-all                       # everything, output and all
.\do.ps1 build-all -Quiet                # one line per project
.\do.ps1 build-all -Apps clock,nupogodi  # just these
.\do.ps1 build-all -NoFirmware           # apps only
.\do.ps1 build-all -NoApps               # firmware only
.\do.ps1 build-all -Clean                # fullclean each project first
.\do.ps1 build-all -NoCcache             # compile everything, every time

.\do.ps1 abi_check apps/*/build/*.app.elf
```

Every app is its own IDF project, so each one compiles the whole IDF — about a
thousand objects and 186 MB of build tree — to link an ELF of a couple of KB
against `libmain.a` and nothing else. The seven sdkconfigs are byte-identical,
so it is one build done seven times, and `build-all` uses ccache to stop paying
for it: measured on this repo, the first app compiles 902 files in 211 s and the
next hits cache on 899 of 916 (**98%**) and takes 78 s. The run prints
`ccache N hit, M compiled` at the end.

Two things that make the difference between 98% and 0%, both found by
measurement and both set by `build-all` itself: `CCACHE_BASEDIR`, so the two
per-app tokens in an otherwise identical compile command (`-I.../<app>/build/config`
and `-fmacro-prefix-map=.../<app>=.`) normalise away; and `CCACHE_NOHASHDIR`,
because the build carries `-ggdb` and ccache otherwise hashes the working
directory, which is a different build tree for every app. It also uses the IDF's
own ccache **by path** — a `ccache.exe` on `PATH` may be another install's broken
shim, and the build then stops on a message about a missing mingw file.

The compiler launcher is baked into `CMakeCache` at configure time, so the first
run after a build tree was configured without ccache reconfigures it.

Apps are found by looking in `apps/`, so a new one is built without editing
anything. An app build **always ends in `FAILED: <name>.elf` / `ld returned 1`,
and that is normal** — apps link `-nostdlib -shared`, so the ordinary firmware
ELF target can never link, and `<name>.app.elf` is finished before it.
`build-all` judges an app by which targets failed, never by the exit code.

By hand it is one IDF project each: `cd neos` or `cd apps/<name>`, then
`idf.py build`.

## Install

| | |
|---|---|
| `flash-os` | The lot: build, flash the firmware, fill the card. |
| `flash` | Build and flash the firmware over USB. Firmware only. |
| `deploy-card` | Copy built apps and `autorun.cfg` onto a card. |

```powershell
.\do.ps1 flash-os                        # checkout -> working tablet
.\do.ps1 flash-os -Quiet -Drive E:
.\do.ps1 flash-os -SkipBuild             # flash + card, no compile
.\do.ps1 flash-os -NoCard -Monitor       # no reader here; stay on the console

.\do.ps1 flash                           # build + flash, port auto-detected
.\do.ps1 flash -Monitor                  # ... and stay on the console
.\do.ps1 flash -Target build             # compile only, no tablet needed
.\do.ps1 flash -Port COM16 -Baud 921600

.\do.ps1 deploy-card                     # apps -> the card, autorun = launcher
.\do.ps1 deploy-card -Drive E: -Autorun hello
.\do.ps1 deploy-card -Autorun ''         # leave the card's autorun.cfg alone
```

Apps do not live in flash — they are ELFs on the card. The partition table is
custom, so a tablet coming from an older build needs a full `flash` and not
just an app image. `deploy-card` deletes nothing: apps not in its list are left
where they are.

An app with a `card/` directory gets its contents copied alongside the ELF, for
the data an app reads off the card rather than carries in its image — so far
that is `lanscan`'s `oui.bin`, built by [genoui](../tools/genoui/). Those files
are generated rather than tracked, so the directory is usually absent, and the
app that wanted it says so instead of failing.

## A running tablet

| | |
|---|---|
| `upload` | Push a file or an app to the card over the console. |
| `launch` | Start an app, without touching `autorun.cfg`. |
| `screencap` | Pull a PNG of the screen. |
| `capture-boot` | Reset and capture the console to stdout. |

```powershell
.\do.ps1 upload --app clock              # built ELF + manifest -> /apps/clock
.\do.ps1 upload --file autorun.cfg
.\do.ps1 upload --file icon.png --as apps/clock/icon.png
.\do.ps1 upload --rm-app clock

.\do.ps1 launch clock                    # run it
.\do.ps1 launch clock --upload           # push it first, then run
.\do.ps1 launch clock --shot --watch 20  # ... a PNG back, and 20 s of console
.\do.ps1 launch --home                   # back to the card's autorun app

.\do.ps1 screencap                       # screencaps/<timestamp>.png
.\do.ps1 screencap --name mandel
.\do.ps1 screencap --out C:/tmp/shot.png

.\do.ps1 capture-boot --secs 40 > boot.log
.\do.ps1 capture-boot --no-reset
```

`launch --upload` is the edit loop: NeOS runs one app at a time and cannot take
the screen back by force, so a launch is a queued request — "started" means the
app is on the screen, and a timeout means whatever was running is not polling
`neos_app_close_requested()`.

Every one of these takes `--port COM16` for a machine with two boards on it.

## Recovery

| | |
|---|---|
| `restore-flash` | Write the stock M5Stack image back to the tablet. |

```powershell
.\do.ps1 restore-flash                   # asks first
.\do.ps1 restore-flash -Port COM16 -Force
```
```bash
./do restore-flash                       # picks the .sh
./do restore-flash --port COM16 --yes
```

The image is `original_flash/tab5-stock-backup.tar.gz`, verified against the
SHA-256 recorded inside the archive before anything is written.
