#pragma once

#include "container.h"
#include "events.h"
#include <algorithm>
#include <initializer_list>
#include <linux/input-event-codes.h>

// Retarget a popup's right click without replaying a press/release or allowing
// ordinary dismissal clicks to activate controls behind the popup.
inline void forward_popup_right_click(Container *root, std::initializer_list<Container *> ignored) {
    for (auto target : pierced_containers(root, root->mouse_current_x, root->mouse_current_y)) {
        if (std::find(ignored.begin(), ignored.end(), target) != ignored.end()) continue;
        if (target->when_clicked) {
            const auto button = target->state.mouse_button_pressed;
            const auto lifetime = std::weak_ptr<bool>(target->lifetime);
            target->state.mouse_button_pressed = BTN_RIGHT;
            target->when_clicked(root, target);
            if (lifetime.lock()) target->state.mouse_button_pressed = button;
        }
        return;
    }
}
