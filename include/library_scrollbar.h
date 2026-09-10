#pragma once
#include <algorithm>

struct LibraryScrollMetrics {
    double maximum = 0;
    double thumb_height = 0;
    double travel = 0;
    double thumb_top = 0;

    LibraryScrollMetrics(double viewport, double maximum_scroll, double track_height,
                         double offset, double minimum_thumb) {
        maximum = std::max(0.0, maximum_scroll);
        track_height = std::max(0.0, track_height);
        thumb_height = viewport + maximum > 0
            ? std::clamp(track_height * viewport / (viewport + maximum),
                         std::min(minimum_thumb, track_height), track_height) : track_height;
        travel = track_height - thumb_height;
        thumb_top = maximum > 0 ? travel * std::clamp(-offset / maximum, 0.0, 1.0) : 0;
    }

    double offset_at(double pointer, double grab_offset) const {
        return travel > 0 ? -maximum * std::clamp((pointer - grab_offset) / travel, 0.0, 1.0) : 0;
    }
};
