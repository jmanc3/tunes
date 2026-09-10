#pragma once
#include <algorithm>
#include <cstdint>
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
    // Import restored/explicit playback lists; normal audio progression only
    // updates the cursor and preserves duplicate identities and priority groups.
    void observe(const std::vector<std::string>& paths, std::size_t index) {
        if (items_.size() != paths.size() || !std::equal(items_.begin(), items_.end(), paths.begin(),
                [](const auto& entry, const auto& path) { return entry.path == path; })) {
            reset(paths, index);
        } else {
            index_ = index < items_.size() ? index : none;
        }
    }
    void reset(const std::vector<std::string>& paths, std::size_t index) {
        items_.clear();
        for (const auto& path : paths) items_.push_back({next_id_++, path});
        index_ = index < items_.size() ? index : none;
    }
    void add(const std::vector<std::string>& paths, Action action) {
        auto at = upcoming_begin();
        if (action == Action::Append) at = items_.size();
        else if (action == Action::AfterNext)
            while (at < items_.size() && items_[at].category == Category::Next) ++at;
        std::vector<Entry> added;
        for (const auto& path : paths)
            added.push_back({next_id_++, path, action == Action::Append ? Category::Queue : Category::Next});
        items_.insert(items_.begin() + at, added.begin(), added.end());
    }
    bool remove(std::uint64_t id) {
        auto it = std::find_if(items_.begin() + upcoming_begin(), items_.end(),
                               [id](const auto& e) { return e.id == id; });
        if (it == items_.end()) return false;
        items_.erase(it);
        return true;
    }
    void clear() { items_.erase(items_.begin() + upcoming_begin(), items_.end()); }
    // Dropping onto another row adopts its category, preserving Next-before-Queue.
    bool move(std::uint64_t id, std::uint64_t target) {
        auto begin = items_.begin() + upcoming_begin();
        auto from = std::find_if(begin, items_.end(), [id](const auto& e) { return e.id == id; });
        auto to = std::find_if(begin, items_.end(), [target](const auto& e) { return e.id == target; });
        if (from == items_.end() || to == items_.end() || from == to) return false;
        auto entry = *from;
        entry.category = to->category;
        const auto offset = to - items_.begin();
        items_.erase(from);
        items_.insert(items_.begin() + offset, std::move(entry));
        return true;
    }
private:
    std::vector<Entry> items_;
    std::size_t index_ = none;
    std::uint64_t next_id_ = 1;
};
