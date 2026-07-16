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

Copy the `.nro` to your SD card (e.g. `sdmc:/switch/angrybirds/angrybirds_nx.nro`), then
place your game files **next to the `.nro`**, in the same folder:

```
sdmc:/switch/angrybirds
├── angrybirds_nx.nro
├── libAngryBirdsClassic.so        <- from your APK: lib/arm64-v8a/
└── assets/                        <- from your APK: the whole assets/ folder
    └── ... (levels, textures, audio, etc.)
```

- `libAngryBirdsClassic.so` — the 64-bit library from `lib/arm64-v8a/` in your
  APK. (32-bit `armeabi-v7a` will not work; this wrapper is arm64.)
- `assets/` — unzip the APK and copy its `assets/` directory verbatim. The
  wrapper reads assets from `.`, `assets/`, and `data/` next to the `.nro`.

Optionally, drop a `cursor.png` (64x64, transparency supported) in the same folder to replace the on-screen cursor with your own.

## Controls
 
| Input | Action |
| --- | --- |
| `+` | Toggle the on-screen cursor |
| `-` | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| `L` / `R` | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| `A` / `ZR` / `ZL` | Tap / confirm (ZL and ZR let you play one-handed) |
| `B` | Back button
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |

A USB mouse works in both handheld and docked: move to control the cursor, left-click to tap, and use the scroll wheel to change 
sensitivity.
Your stick, mouse and gyro sensitivities are remembered in `pointer.cfg` automatically after in-game adjustment.

## Languages
On first launch the wrapper writes `sdmc:/switch/angrybirds/config.txt`
(one `name value` per line, `#` for comments):
 
```
# language: 'auto' follows the Switch system language, or one of the codes in
# the table below (e.g. fr, de, es, es_419, pt, ru, ja, zh, zh_TW). 
```

| `language=` | Game locale | UI text |
|---|---|---|
| `auto` | follows the Switch system language | — |
| `en` | en_EN | English (no patch — stock) |
| `fr` | fr_FR | French |
| `it` | it_IT | Italian |
| `de` | de_DE | German |
| `es` | es_ES | Spanish (Spain) |
| `es_419` | es_419 | Latin-American Spanish |
| `pt` | pt_BR | Brazilian Portuguese |
| `ru` | ru_RU | Russian |
| `ja` | ja_JP | Japanese |
| `zh` | zh_CN | Simplified Chinese |
| `zh_TW` | zh_TW | Traditional Chinese |


## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
**switch-dev** group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-freetype switch-libpng switch-zlib switch-bzip2
```


## Credits

The loader/shim infrastructure (`so_util`, `libc_shim`, `util`, `error`) derives
from the open-source Switch port of *Burger Shop* by Andy Nguyen, fgsfds and
ChanseyIsTheBest, which in turn builds on TheOfficialFloW's Vita/Switch loader
lineage — all MIT-licensed. The Fusion-specific JNI, platform callbacks, audio,
imports and main loop in this project are new. Thanks to everyone in that
lineage for making this approach possible.

The on-boot language patcher vendors two public-domain libraries, unmodified
except for build configuration: the **LZMA SDK** by Igor Pavlov
(`LzmaDec.c`, `LzmaEnc.c`, `LzFind.c`, `Alloc.c` and headers) for the game's
script (de)compression, and **tiny-AES-c** by kokke (`aes.c`/`aes.h`, set to
AES-256) for the AES-CBC layer. Both are public domain; the sources live in
`source/` alongside the wrapper. `lzma_alone.c`, `patch_bytecode.c` and
`locale_patch.c` (the LZMA-alone framing, Lua-bytecode edit, and boot logic) are
new to this project.
