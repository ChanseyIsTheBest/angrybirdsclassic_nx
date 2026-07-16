/* config.h -- build-time constants for the Angry Birds Classic Switch port
 *
 * Angry Birds Classic (com.rovio.angrybirds) runs on Rovio's "Fusion" engine.
 * Unlike the two-module Burger Shop donor (engine + BASS), Fusion ships as a
 * single native library that renders with GLES2 and mixes its own audio (it
 * pulls PCM through AudioOutput.nativeMixData). There is no separate audio .so
 * and no external PAK: the engine reads its data through the Android
 * AssetManager, which we emulate over loose files in the game directory.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The single native module extracted from the APK's lib/arm64-v8a/.
#define SO_NAME  "libAngryBirdsClassic.so"

// Where the log and the crash breadcrumb are written. main() creates this
// directory at startup so the writes can't fail if it doesn't exist yet.
#define DATA_DIR "sdmc:/switch/angrybirds"
#define LOG_NAME DATA_DIR "/angrybirds_nx.log"

// Address-space split (see __libnx_initheap in main.c). We reserve a fixed zone
// to hold the loaded .so (code + data + the temporary file image used during
// relocation) and hand *everything else* to the newlib heap, which is where the
// engine's malloc goes (decoded textures, audio buffers, Lua/Box2D state). The
// library is ~8 MB; 96 MB is comfortable headroom for it plus its relocation
// scratch. Do NOT set this so large that little is left for the game.
#define SO_ZONE_MB 96

// Per-line SD-card writes are slow, but during bring-up visibility matters more
// than frame time, so logging is ON by default. Set to 0 once the game is
// stable. (Independent of this, main() always writes a tiny stage breadcrumb to
// sdmc:/angrybirds_nx_stage.txt so a crash still tells us how far it got.)
#define DEBUG_LOG 0

// Assets are looked up relative to these roots, in order. Drop the APK's
// assets/ folder next to the .nro; the engine asks for paths like
// "data/..." which resolve under ./ or ./assets/.
// Asset lookup roots, tried in order under the game directory. On Android,
// AAssetManager_open("data/x") means "assets/data/x", so "assets" comes first;
// "." and "data" cover titles laid out without the assets/ wrapper.
#define ASSET_ROOTS { "assets", ".", "data" }

// Render size is fixed at 1080p in both docked and handheld for a consistent
// layout; in handheld the console downscales to the 720p panel. The touch panel
// still reports in its own 1280x720 space, so touch is scaled into render space.
extern int screen_width;
extern int screen_height;

// ---- user config file (sdmc:/switch/angrybirds/config.txt) ----
// A simple "name value" text file, created with defaults on first run.
#define CONFIG_NAME DATA_DIR "/config.txt"

typedef struct {
  char language[8];    // "auto" or a 2-letter code (en, de, fr, es, it, ...)
} Config;

extern Config config;

int read_config(const char *file);   // 0 ok, 1 ok-but-rewrite, -1 missing
int write_config(const char *file);

#endif
