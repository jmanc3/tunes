#pragma once

#include "container.h"
#include "client/raw_windowing.h"
#include "utility.h"

struct MylarWindow {
    RGBA bg_color{1, 1, 1, 1};
    Container *root = nullptr;
    RawWindow *raw_window = nullptr;
    MylarWindow *popup_window = nullptr;
};

MylarWindow *open_mylar_window(RawApp *app, WindowType type, RawWindowSettings settings);
MylarWindow *open_mylar_popup(MylarWindow *parent, RawWindowSettings settings);
