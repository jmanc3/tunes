#pragma once

#include "drawing/color.h"

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

inline const Theme light_theme {
    {.98,.985,.99,1}, // surface
    {.87,.94,.97,1}, // button
    {.16,.22,.26,1}, // text
    {.16,.22,.26,.12}, // divider
    {.91,.95,.97,1}, // selection
    {.02,.56,.73,1}, // accent
    {.65,.16,.17,1}, // danger_text
    {.9,.91,.93,1}, // placeholder
    {.45,.47,.5,1}, // placeholder_text
    {0,0,0,1}, // text_primary
    {.4,.4,.4,1}, // text_muted
    {.66,.73,.77,1}, // disabled
    {.02,.52,.68,1}, // accent_hover
    {.24,.34,.40,1}, // icon
    {.76,.84,.88,1}, // button_disabled
    {.02,.43,.58,1}, // accent_pressed
    {.04,.62,.79,1}, // accent_fill
    {.80,.86,.89,1}, // track
    {.96,.98,.99,1}, // playback_surface
    {.82,.88,.91,1}, // border
    {.38,.47,.53,1}, // body
    {.12,.22,.29,1}, // heading
    {.87,.93,.96,1}, // art_placeholder
    {.20,.52,.64,1}, // art_placeholder_icon
    {.05,.12,.17,.48}, // scrim
    {.97,.99,1,1}, // panel
    {.02,.39,.53,1}, // status
    {.65,.16,.12,1}, // error
    {.78,.91,.96,1}, // button_hover
    {.45,.52,.56,1}, // disabled_text
    {1,1,1,1},
    {1,1,1,1},
    {.2,.3,.36,1},
};

// Neutral grays for dark chrome; blue is reserved for accents.
inline const Theme dark_theme {
    {0.126,0.126,0.126,1}, // surface
    {0.237,0.237,0.237,1}, // button
    {0.914,0.914,0.914,1}, // text
    {0.834,0.834,0.834,.16}, // divider
    {0.207,0.207,0.207,1}, // selection
    {.15,.72,.88,1}, // accent
    {.98,.43,.44,1}, // danger_text
    {0.207,0.207,0.207,1}, // placeholder
    {0.655,0.655,0.655,1}, // placeholder_text
    {0.947,0.947,0.947,1}, // text_primary
    {0.703,0.703,0.703,1}, // text_muted
    {0.409,0.409,0.409,1}, // disabled
    {.28,.80,.94,1}, // accent_hover
    {0.797,0.797,0.797,1}, // icon
    {0.291,0.291,0.291,1}, // button_disabled
    {.10,.59,.75,1}, // accent_pressed
    {.04,.50,.65,1}, // accent_fill
    {0.319,0.319,0.319,1}, // track
    {0.116,0.116,0.116,1}, // playback_surface
    {0.299,0.299,0.299,1}, // border
    {0.736,0.736,0.736,1}, // body
    {0.934,0.934,0.934,1}, // heading
    {0.221,0.221,0.221,1}, // art_placeholder
    {.35,.72,.83,1}, // art_placeholder_icon
    {0,0,0,.65}, // scrim
    {0.154,0.154,0.154,1}, // panel
    {.36,.80,.92,1}, // status
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
inline const RGBA drop_indicator{.2, .35, .42, .4};
inline const RGBA edit_background{.6, .6, .6, .18};
inline const RGBA text_selection{.25, .6, .9, .4};
inline const RGBA art_text_dark{.08, .08, .08, 1};
inline const RGBA art_text_light{.98, .98, .98, 1};
inline const RGBA row_hover{0, 0, 0, .08};
inline const RGBA row_track{0, 0, 0, .07};
inline const RGBA remove_hover{.85, .2, .2, .15};
inline const RGBA remove_icon{.85, .2, .2, 1};
inline const RGBA playing_indicator{.2, .57, .88, 1};
inline const RGBA art_error{.2, .05, .05, .92};
inline const RGBA art_overlay{0, 0, 0, .65};
inline const RGBA preview_scrim{0, 0, 0, .82};
inline const RGBA progress_surface{.15, .15, .15, .97};
inline const RGBA progress_fill{.4, .8, .95, 1};
inline const RGBA black{0, 0, 0, 1};
inline const RGBA album_fallback{.95, .96, .97, 1};
inline const RGBA album_secondary{.4, .4, .4, 1};
inline const RGBA folder_icon{.12, .34, .43, 1};
inline const RGBA folder_icon_hover{.12, .56, .43, 1};
}
