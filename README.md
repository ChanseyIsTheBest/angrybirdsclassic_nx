# Angry Birds Classic — Nintendo Switch port (Fusion engine wrapper)

This is a **native wrapper / loader** that runs the original ARM64 build of
*Angry Birds Classic* on Switch homebrew. It contains **no game code and no game
assets**. It loads the game's own native library (`libAngryBirdsClassic.so`) and
recreates, natively, the thin Android/JNI layer the engine expects — a fake
`JNIEnv`/`JavaVM`, a GLES2 context, an audio device, input, and the handful of
"Java wrapper" callbacks the engine makes. You supply the library and the assets
from a copy of the game **you legally own**.

## Install & run

You need files from "Angry_Birds_Classic_7.0.0.apk"

Copy the `.nro` to your SD card (e.g. `sdmc:/switch/angrybirds_nx.nro`), then
place your game files **next to the `.nro`**, in the same folder:

```
sdmc:/switch/
├── angrybirds_nx.nro
├── libAngryBirdsClassic.so        <- from your APK: lib/arm64-v8a/
└── assets/                        <- from your APK: the whole assets/ folder
    └── ... (levels, textures, audio, etc.)
```

- `libAngryBirdsClassic.so` — the 64-bit library from `lib/arm64-v8a/` in your
  APK. (32-bit `armeabi-v7a` will not work; this wrapper is arm64.)
- `assets/` — unzip the APK and copy its `assets/` directory verbatim. The
  wrapper reads assets from `.`, `assets/`, and `data/` next to the `.nro`.

## Controls

In **handheld mode** the game is driven by the touchscreen, exactly like Android.

In **docked mode** (no touchscreen) the left stick drives an on-screen cursor
and **A** taps — press-and-hold **A** while moving the stick to drag (this is how
you pull back and aim the slingshot). A small crosshair shows the cursor
position. **B** or **+** send the Android *Back* key (cancel / menu-back). The
**HOME** button suspends/exits as usual.

## Requirements (to build)

Install devkitPro with the Switch toolchain and these packages:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2
```

`switch-mesa` provides GLES2/EGL; `switch-sdl2` backs the audio device.

## Credits

The loader/shim infrastructure (`so_util`, `libc_shim`, `util`, `error`) derives
from the open-source Switch port of *Burger Shop* by Andy Nguyen, fgsfds and
ChanseyIsTheBest, which in turn builds on TheOfficialFloW's Vita/Switch loader
lineage — all MIT-licensed. The Fusion-specific JNI, platform callbacks, audio,
imports and main loop in this project are new. Thanks to everyone in that
lineage for making this approach possible.
