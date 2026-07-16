#ifndef LOCALE_PATCH_H
#define LOCALE_PATCH_H

// Called once at boot (after config load, after g_asset_base is set).
// If config.language (or the Switch system language, when "auto") maps to one of
// the game's supported non-English locales, this decrypts the stock
// gamelogic.lua, injects a getCurrentLocale override forcing that locale,
// re-encrypts it to DATA_DIR/gamelogic_patched.lua, and arms a redirect so the
// game loads the patched script. English/unsupported -> no patch (stock script).
void locale_patch_init(void);

// Non-empty host path to the patched gamelogic.lua once a patch is active,
// otherwise "" . Read by resolve_asset_path() to redirect the script load.
extern char g_patched_gamelogic[512];

#endif
