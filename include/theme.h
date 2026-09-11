#pragma once

#include "drawing/color.h"

// Base accent: soft purple #A78BFA, normalized for the renderer.
namespace accent_colors {
inline const RGBA base{167 / 255.0, 139 / 255.0, 250 / 255.0, 1};
inline const RGBA hover{153 / 255.0, 120 / 255.0, 240 / 255.0, 1};
inline const RGBA pressed{139 / 255.0, 101 / 255.0, 224 / 255.0, 1};
inline const RGBA text_light{109 / 255.0, 40 / 255.0, 217 / 255.0, 1};
inline const RGBA text_dark{196 / 255.0, 181 / 255.0, 253 / 255.0, 1};
inline const RGBA foreground{.06, .06, .06, 1};
inline const RGBA selection{167 / 255.0, 139 / 255.0, 250 / 255.0, .4};
}

// Application chrome palettes. Artwork colors remain content-derived.
struct Theme {
    RGBA surface;
    RGBA button;
    RGBA text;
    RGBA divider;
    RGBA selection;
    RGBA accent;
    RGBA danger_text;
    RGBA placeholder;
    RGBA placeholder_text;
    RGBA text_primary;
    RGBA text_muted;
    RGBA disabled;
    RGBA accent_hover;
    RGBA icon;
    RGBA button_disabled;
    RGBA accent_pressed;
    RGBA accent_fill;
    RGBA track;
    RGBA playback_surface;
    RGBA border;
    RGBA body;
    RGBA heading;
    RGBA art_placeholder;
    RGBA art_placeholder_icon;
    RGBA scrim;
    RGBA panel;
    RGBA status;
    RGBA error;
    RGBA button_hover;
    RGBA disabled_text;
    RGBA background;
    RGBA on_accent;
    RGBA scrollbar;
};

// Neutral grays for light chrome; purple is reserved for accents.
inline const Theme light_theme {
    {0.984,0.984,0.984,1}, // surface
    {0.927,0.927,0.927,1}, // button
    {0.21,0.21,0.21,1}, // text
    {0.21,0.21,0.21,.12}, // divider
    {0.943,0.943,0.943,1}, // selection
    accent_colors::base, // accent
    {.65,.16,.17,1}, // danger_text
    {0.909,0.909,0.909,1}, // placeholder
    {0.468,0.468,0.468,1}, // placeholder_text
    {0,0,0,1}, // text_primary
    {0.4,0.4,0.4,1}, // text_muted
    {0.718,0.718,0.718,1}, // disabled
    accent_colors::hover, // accent_hover
    {0.323,0.323,0.323,1}, // icon
    {0.826,0.826,0.826,1}, // button_disabled
    accent_colors::pressed, // accent_pressed
    accent_colors::base, // accent_fill
    {0.849,0.849,0.849,1}, // track
    {0.976,0.976,0.976,1}, // playback_surface
    {0.869,0.869,0.869,1}, // border
    {0.455,0.455,0.455,1}, // body
    {0.204,0.204,0.204,1}, // heading
    {0.919,0.919,0.919,1}, // art_placeholder
    accent_colors::text_light, // art_placeholder_icon
    {0.109,0.109,0.109,.48}, // scrim
    {0.986,0.986,0.986,1}, // panel
    accent_colors::text_light, // status
    {.65,.16,.12,1}, // error
    {0.886,0.886,0.886,1}, // button_hover
    {0.508,0.508,0.508,1}, // disabled_text
    {1,1,1,1},
    {1,1,1,1},
    {0.283,0.283,0.283,1},
};

// Neutral grays for dark chrome; purple is reserved for accents.
inline const Theme dark_theme {
    {0.126,0.126,0.126,1}, // surface
    {0.237,0.237,0.237,1}, // button
    {0.914,0.914,0.914,1}, // text
    {0.834,0.834,0.834,.16}, // divider
    {0.207,0.207,0.207,1}, // selection
    accent_colors::base, // accent
    {.98,.43,.44,1}, // danger_text
    {0.207,0.207,0.207,1}, // placeholder
    {0.655,0.655,0.655,1}, // placeholder_text
    {0.947,0.947,0.947,1}, // text_primary
    {0.703,0.703,0.703,1}, // text_muted
    {0.409,0.409,0.409,1}, // disabled
    accent_colors::hover, // accent_hover
    {0.797,0.797,0.797,1}, // icon
    {0.291,0.291,0.291,1}, // button_disabled
    accent_colors::pressed, // accent_pressed
    accent_colors::base, // accent_fill
    {0.319,0.319,0.319,1}, // track
    {0.116,0.116,0.116,1}, // playback_surface
    {0.299,0.299,0.299,1}, // border
    {0.736,0.736,0.736,1}, // body
    {0.934,0.934,0.934,1}, // heading
    {0.221,0.221,0.221,1}, // art_placeholder
    accent_colors::text_dark, // art_placeholder_icon
    {0,0,0,.65}, // scrim
    {0.154,0.154,0.154,1}, // panel
    accent_colors::text_dark, // status
    {1,.48,.40,1}, // error
    {0.312,0.312,0.312,1}, // button_hover
    {0.537,0.537,0.537,1}, // disabled_text
    {0.083,0.083,0.083,1},
    {1,1,1,1},
    {0.742,0.742,0.742,1},
};

inline bool dark_theme_enabled = false;
inline const Theme &theme() { return dark_theme_enabled ? dark_theme : light_theme; }
inline RGBA with_alpha(RGBA color, double alpha) { color.a = alpha; return color; }

// Fixed colors for shadows, artwork overlays, and content-derived album palettes.
namespace theme_colors {
inline const RGBA shadow{0, 0, 0, .4};
inline const RGBA shadow_soft{0, 0, 0, .2};
inline const RGBA drop_indicator = accent_colors::selection;
inline const RGBA edit_background{.6, .6, .6, .18};
inline const RGBA text_selection = accent_colors::selection;
inline const RGBA art_text_dark{.08, .08, .08, 1};
inline const RGBA art_text_light{.98, .98, .98, 1};
inline const RGBA row_hover{0, 0, 0, .08};
inline const RGBA row_track{0, 0, 0, .07};
inline const RGBA remove_hover{.85, .2, .2, .15};
inline const RGBA remove_icon{.85, .2, .2, 1};
inline const RGBA playing_indicator = accent_colors::base;
inline const RGBA art_error{.2, .05, .05, .92};
inline const RGBA art_overlay{0, 0, 0, .65};
inline const RGBA preview_scrim{0, 0, 0, .82};
inline const RGBA progress_surface{.15, .15, .15, .97};
inline const RGBA progress_fill = accent_colors::base;
inline const RGBA black{0, 0, 0, 1};
inline const RGBA album_fallback{.96, .96, .96, 1};
inline const RGBA album_secondary{.4, .4, .4, 1};
inline const RGBA folder_icon = accent_colors::text_light;
inline const RGBA folder_icon_hover = accent_colors::hover;
}
