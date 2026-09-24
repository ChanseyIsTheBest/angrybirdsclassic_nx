#ifndef LOCALE_PATCH_H
#define LOCALE_PATCH_H

// Called once at boot (after config load, after g_asset_base is set).
// If config.language (or the Switch system language, when "auto") maps to one of
// the game's supported non-English locales, this decrypts the stock
// gamelogic.lua, injects a getCurrentLocale override forcing that locale,
// re-encrypts it to DATA_DIR/gamelogic_patched.lua, and arms a redirect so the
// game loads the patched script. For Simplified Chinese it also switches the
// boot splash and title menu to the Chinese logo (splashes_patched.lua,
// main_menu_patched.lua, and a fixed 1024x600 splash composite table).
// English/unsupported -> no patch (stock files).
void locale_patch_init(void);

// If `rel` (an engine-relative asset path such as
// "data/scripts_common/gamelogic.lua") has been replaced by a patched file,
// return that file's host path; otherwise NULL. Always NULL until
// locale_patch_init() has produced a patch, so its own reads of the stock files
// are never redirected.
const char *locale_patch_redirect(const char *rel);

#endif
