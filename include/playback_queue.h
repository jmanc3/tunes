#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

// UI-owned ordering, independent of audio decoding. History stays in the timeline
// so Previous works without shifting the player's current item during edits.
class PlaybackQueue {
public:
    enum class Category { Next, Queue };
    enum class Action { PlayNext, AfterNext, Append };
    struct Entry {
        std::uint64_t id;
        std::string path;
        Category category = Category::Queue;
        // Playback provenance, saved with the session rather than track metadata.
        std::string playlist_id;
        // Fed from the continuation; always kept after explicitly queued items.
        bool continuation = false;
    };
    static constexpr auto none = std::numeric_limits<std::size_t>::max();
    const std::vector<Entry>& entries() const { return items_; }
    const Entry* current() const { return index_ < items_.size() ? &items_[index_] : nullptr; }
    std::size_t upcoming_begin() const { return current() ? index_ + 1 : 0; }
    std::vector<std::string> paths() const {
        std::vector<std::string> result;
        for (const auto& item : items_) result.push_back(item.path);
        return result;
    }
    std::vector<std::string> playlist_ids() const {
        std::vector<std::string> result;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].playlist_id.empty()) continue;
            if (result.empty()) result.resize(items_.size());
            result[i] = items_[i].playlist_id;
        }
        return result;
    }
    // Import restored/explicit playback lists; normal audio progression only
    // updates the cursor and preserves duplicate identities and priority groups.
    template<typename SameTrack = std::equal_to<>>
    void observe(const std::vector<std::string>& paths, std::size_t index, SameTrack same_track = {}) {
        if (items_.size() != paths.size() || !std::equal(items_.begin(), items_.end(), paths.begin(),
                [&](const auto& entry, const auto& path) { return same_track(entry.path, path); })) {
            reset(paths, index);
        } else {
            // Audio conversion may change a path without replacing its queue entry.
            for (std::size_t i = 0; i < paths.size(); ++i) items_[i].path = paths[i];
            index_ = index < items_.size() ? index : none;
        }
    }
    // A new playback context replaces any continuation.
    void reset(const std::vector<std::string>& paths, std::size_t index, const std::string& playlist_id = {}) {
        items_.clear();
        continuation_.clear();
        for (const auto& path : paths) items_.push_back({next_id_++, path, Category::Queue, playlist_id});
        index_ = index < items_.size() ? index : none;
    }
    void restore(const std::vector<std::string>& paths, std::size_t index,
                 const std::vector<std::string>& playlist_ids) {
        reset(paths, index);
        for (std::size_t i = 0; i < std::min(items_.size(), playlist_ids.size()); ++i)
            items_[i].playlist_id = playlist_ids[i];
    }
    void add(const std::vector<std::string>& paths, Action action, const std::string& playlist_id = {}) {
        auto at = upcoming_begin();
        if (action == Action::Append) {
            // Explicitly queued songs play before the continuation.
            at = items_.size();
            while (at > upcoming_begin() && items_[at - 1].continuation) --at;
        } else if (action == Action::AfterNext)
            while (at < items_.size() && items_[at].category == Category::Next) ++at;
        std::vector<Entry> added;
        for (const auto& path : paths)
            added.push_back({next_id_++, path, action == Action::Append ? Category::Queue : Category::Next, playlist_id});
        items_.insert(items_.begin() + at, added.begin(), added.end());
    }
    bool remove(std::uint64_t id) {
        auto it = std::find_if(items_.begin() + upcoming_begin(), items_.end(),
                               [id](const auto& e) { return e.id == id; });
        if (it == items_.end()) return false;
        items_.erase(it);
        return true;
    }
    void clear() {
        items_.erase(items_.begin() + upcoming_begin(), items_.end());
        continuation_.clear();
    }

    // The continuation is the rest of a list (such as the Songs page) that plays
    // after everything queued. Only one of its tracks is in the timeline at a
    // time, so a long list never reaches the player (or its converter) at once.
    void set_continuation(std::vector<std::string> paths) {
        remove_continuation();
        continuation_ = std::move(paths);
    }
    // Moves the next continuation track into the timeline when none is waiting.
    bool feed() {
        if (continuation_.empty() || continuation_waiting()) return false;
        items_.push_back({next_id_++, continuation_.front(), Category::Queue, {}, true});
        continuation_.erase(continuation_.begin());
        return true;
    }
    // Upcoming continuation tracks, including the one already in the timeline.
    std::size_t continuation_size() const {
        return continuation_.size() + static_cast<std::size_t>(std::count_if(
            items_.begin() + upcoming_begin(), items_.end(), [](const auto& e) { return e.continuation; }));
    }
    std::string continuation_front() const {
        for (auto i = upcoming_begin(); i < items_.size(); ++i)
            if (items_[i].continuation) return items_[i].path;
        return continuation_.empty() ? std::string{} : continuation_.front();
    }
    bool remove_continuation() {
        const auto before = items_.size();
        items_.erase(std::remove_if(items_.begin() + upcoming_begin(), items_.end(),
                                    [](const auto& e) { return e.continuation; }), items_.end());
        const bool removed = before != items_.size() || !continuation_.empty();
        continuation_.clear();
        return removed;
    }
    // Dropping onto another row adopts its category, preserving Next-before-Queue.
    bool move(std::uint64_t id, std::uint64_t target) {
        auto begin = items_.begin() + upcoming_begin();
        auto from = std::find_if(begin, items_.end(), [id](const auto& e) { return e.id == id; });
        auto to = std::find_if(begin, items_.end(), [target](const auto& e) { return e.id == target; });
        if (from == items_.end() || to == items_.end() || from == to) return false;
        // The continuation is one unit; it cannot be reordered or dropped onto.
        if (from->continuation || to->continuation) return false;
        auto entry = *from;
        entry.category = to->category;
        const auto offset = to - items_.begin();
        items_.erase(from);
        items_.insert(items_.begin() + offset, std::move(entry));
        return true;
    }
private:
    bool continuation_waiting() const {
        return std::any_of(items_.begin() + upcoming_begin(), items_.end(), [](const auto& e) { return e.continuation; });
    }
    std::vector<Entry> items_;
    std::vector<std::string> continuation_;
    std::size_t index_ = none;
    std::uint64_t next_id_ = 1;
};
