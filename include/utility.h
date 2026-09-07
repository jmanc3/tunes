#pragma once

#include "container.h"
#include <cairo/cairo.h>
#include <cassert>

struct RGBA  {
    double r = 0;
    double g = 0;
    double b = 0;
    double a = 0;
    
    RGBA() {};

    RGBA(std::string hex) {
        
    }
    
    RGBA(double r, double g, double b, double a) {
        this->r = r;
        this->g = g;
        this->b = b;
        this->a = a;
    }
};

struct Timer {
    
};

#define center_y(c, in_h) (c->real_bounds.y + c->real_bounds.h * .5) - (in_h * .5)
#define center_x(c, in_w) c->real_bounds.x + c->real_bounds.w * .5 - in_w * .5
#define paint [](Container *root, Container *c)
#define fz std::format


static void later(float time_ms, std::function<void(Timer *)> func) {
    
}

static void animate(float *value, float target, float time_ms, std::shared_ptr<bool> lifetime, std::function<void(bool)> on_completion = nullptr, std::function<float(float)> lerp_func = nullptr, float delay = 0.0) {
}

static void set_argb(cairo_t *cr, RGBA color) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
}

static void set_rect(cairo_t *cr, Bounds bounds) {
    cairo_rectangle(cr, bounds.x, bounds.y, bounds.w, bounds.h);
}

static void log(std::string args) {
    
}
