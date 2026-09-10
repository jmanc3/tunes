#include "container.h"
#include "events.h"
#include "popup_input.h"
#include <linux/input-event-codes.h>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static void click(Container *root, int x, int y, int button = BTN_LEFT) {
    move_event(root, Event(x, y));
    mouse_event(root, Event(x, y, button, 1));
    mouse_event(root, Event(x, y, button, 0));
}

int main() {
    Container root;
    root.type = ::fullycustom;
    root.wanted_bounds = Bounds(0, 0, FILL_SPACE, FILL_SPACE);
    auto library = root.child(FILL_SPACE, FILL_SPACE);
    auto card = library->child(FILL_SPACE, FILL_SPACE);
    auto button = root.child(FILL_SPACE, FILL_SPACE);
    auto overlay = root.child(FILL_SPACE, FILL_SPACE);
    overlay->type = ::fullycustom;
    overlay->z_index = 90;
    overlay->exists = false;
    Bounds menu;
    int library_clicks = 0, menu_clicks = 0;
    overlay->pre_layout = [&](Container *, Container *, const Bounds &b) {
        menu = Bounds(b.right() - 200, b.y + 20, 180, 200);
    };
    root.pre_layout = [&](Container *r, Container *, const Bounds &b) {
        layout(r, library, b);
        layout(r, button, Bounds(350, 350, 40, 40));
        if (overlay->exists) layout(r, overlay, b);
    };
    button->when_clicked = [&](Container *r, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT) return;
        overlay->exists = !overlay->exists;
        layout(r, r, r->real_bounds);
    };
    card->when_clicked = [&](Container *r, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT) ++library_clicks;
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            overlay->exists = true;
            layout(r, r, r->real_bounds);
        }
    };
    overlay->when_clicked = [&](Container *r, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT) return;
        if (bounds_contains(menu, r->mouse_current_x, r->mouse_current_y)) ++menu_clicks;
        c->exists = false;
    };
    layout(&root, &root, Bounds(0, 0, 400, 400));
    click(&root, 370, 370);
    check(overlay->exists, "queue click must open the overlay");
    check(menu.w == 180 && menu.h == 200, "custom leaf overlay must calculate its menu bounds");
    check(pierced_containers(&root, 250, 50).front() == overlay,
          "overlay must receive input before library descendants");
    click(&root, 250, 50);
    check(menu_clicks == 1 && library_clicks == 0, "menu selection must not click through");
    click(&root, 50, 50, BTN_RIGHT);
    check(overlay->exists, "right click must open a context menu");
    move_event(&root, Event(250, 50));
    check(overlay->state.mouse_hovering && !card->state.mouse_hovering,
          "context menu must own hover over underlying tracks");
    move_event(&root, Event(260, 70));
    check(!card->state.mouse_hovering, "moving inside the menu must not hover tracks");
    click(&root, 50, 50);
    check(!overlay->exists && library_clicks == 0, "outside click must dismiss without click-through");
    click(&root, 50, 50);
    check(library_clicks == 1, "library input must recover after dismissal");
    check(card->state.mouse_hovering, "track hover must recover after dismissal");

    // Input must reverse the paint order, including equal-z siblings.
    overlay->exists = true;
    auto peer = root.child(FILL_SPACE, FILL_SPACE);
    peer->z_index = overlay->z_index;
    peer->real_bounds = root.real_bounds;
    Container *last_painted = nullptr;
    overlay->when_paint = [&](Container *, Container *c) { last_painted = c; };
    peer->when_paint = overlay->when_paint;
    paint_outline(&root, &root);
    check(last_painted == peer && pierced_containers(&root, 50, 50).front() == peer,
          "equal-z input and painting must agree");
    peer->z_index = 1;
    check(pierced_containers(&root, 50, 50).front() == overlay,
          "z-index must take precedence over insertion order");

    // Replacing a context popup must reach the new target through both popup
    // layers, while ordinary outside clicks still dismiss without click-through.
    peer->exists = false;
    auto context = root.child(FILL_SPACE, FILL_SPACE);
    context->z_index = 91;
    context->real_bounds = root.real_bounds;
    context->exists = false;
    const Bounds context_bounds(20, 250, 160, 80);
    int context_opens = 0;
    card->when_clicked = [&](Container *, Container *c) {
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            ++context_opens;
            context->exists = true;
        } else if (c->state.mouse_button_pressed == BTN_LEFT) ++library_clicks;
    };
    overlay->when_clicked = [&](Container *r, Container *c) {
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            forward_popup_right_click(r, {overlay, context});
        } else if (c->state.mouse_button_pressed == BTN_LEFT) {
            if (bounds_contains(menu, r->mouse_current_x, r->mouse_current_y)) ++menu_clicks;
            else c->exists = context->exists = false;
        }
    };
    context->when_clicked = [&](Container *r, Container *c) {
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            c->exists = false;
            forward_popup_right_click(r, {context, overlay});
        } else if (c->state.mouse_button_pressed == BTN_LEFT &&
                   !bounds_contains(context_bounds, r->mouse_current_x, r->mouse_current_y)) {
            c->exists = overlay->exists = false;
        }
    };
    click(&root, 50, 50, BTN_RIGHT);
    check(overlay->exists && context->exists && context_opens == 1,
          "right click through queue must open context without closing queue");
    click(&root, 80, 80, BTN_RIGHT);
    check(overlay->exists && context->exists && context_opens == 2,
          "a second right click must replace context in one click");
    check(card->state.mouse_button_pressed == 0,
          "forwarding must restore the target's button state");
    check(pierced_containers(&root, 50, 50).front() == context,
          "context must remain above queue");
    click(&root, 50, 50);
    check(!overlay->exists && !context->exists && library_clicks == 1,
          "outside click must dismiss both without activating library");
    click(&root, 50, 50, BTN_RIGHT);
    click(&root, 80, 80, BTN_RIGHT);
    check(context->exists && !overlay->exists && context_opens == 4,
          "context replacement must also work without an open queue");
    overlay->exists = true;
    click(&root, 250, 50);
    check(!overlay->exists && !context->exists && menu_clicks == 1,
          "clicking queue outside context must dismiss both without selecting queue");
}
