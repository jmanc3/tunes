#include "container.h"
#include "events.h"
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    for (bool mouse : {false, true}) {
        Container root;
        root.real_bounds = Bounds(0, 0, 400, 400);
        root.receive_events_even_if_obstructed = true;
        root.when_fine_scrolled = [](Container *, Container *, double, double, bool) {};
        auto first = root.child(100, 100);
        auto second = root.child(100, 100);
        first->real_bounds = Bounds(0, 0, 100, 100);
        second->real_bounds = Bounds(0, 200, 100, 100);
        int enters = 0;
        second->when_mouse_enters_container = [&](Container *, Container *) { ++enters; };
        move_event(&root, Event(50, 50));
        check(first->state.mouse_hovering, "initial card should hover");

        // A scroll frame lays out another card underneath the stationary pointer.
        first->real_bounds.y = -200;
        second->real_bounds.y = 0;
        Event scroll(50, 50, 0, 0);
        scroll.scroll = true;
        scroll.delta = 1;
        scroll.from_mouse = mouse;
        mouse_event(&root, scroll);
        move_event(&root, Event(51, 51));
        check(second->state.mouse_hovering, "hover must recover without leaving the card");
        check(!first->state.mouse_hovering, "old card must lose hover");
        check(enters == 1, "new card must receive one hover entry");
        mouse_event(&root, scroll);
        move_event(&root, Event(52, 52));
        check(enters == 1, "continued scrolling must not repeat hover entry");
    }
}
