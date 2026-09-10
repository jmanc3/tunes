#pragma once

#include "container.h"
#include "drawing/context.h"
#include <cassert>

#define center_y(c, in_h) (c->real_bounds.y + c->real_bounds.h * .5) - (in_h * .5)
#define center_x(c, in_w) c->real_bounds.x + c->real_bounds.w * .5 - in_w * .5
#define paint [](Container *root, Container *c)
#define fz std::format
#define BTN_LEFT		0x110
#define BTN_RIGHT		0x111
#define BTN_MIDDLE		0x112

struct Timer {
    
};

static void later(float time_ms, std::function<void(Timer *)> func) {
    
}

static void animate(float *value, float target, float time_ms, std::shared_ptr<bool> lifetime, std::function<void(bool)> on_completion = nullptr, std::function<float(float)> lerp_func = nullptr, float delay = 0.0) {
}

static void set_argb(drawing::Context *cr, RGBA color) {
    cr->set_color(color);
}

static void set_rect(drawing::Context *cr, Bounds bounds) {
    cr->rectangle(bounds.x, bounds.y, bounds.w, bounds.h);
}

static void log(std::string args) {
    //printf(args.c_str());
    //printf("\n");
}
