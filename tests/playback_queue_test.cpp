#include "playback_queue.h"
#include <stdexcept>
#include <iostream>
static void check(bool ok) { if (!ok) throw std::runtime_error("queue invariant failed"); }
int main() {
    using Q = PlaybackQueue;
    Q q;
    q.observe({"history", "current", "album-tail"}, 1);
    auto current = q.current()->id;
    q.add({"next-a", "next-b"}, Q::Action::PlayNext);
    q.add({"after-a", "after-b"}, Q::Action::AfterNext);
    q.add({"urgent"}, Q::Action::PlayNext);
    q.add({"last", "last"}, Q::Action::Append);
    check(q.paths() == std::vector<std::string>({"history", "current", "urgent", "next-a", "next-b", "after-a", "after-b", "album-tail", "last", "last"}));
    check(q.current()->id == current);
    auto duplicate = q.entries().back().id;
    check(q.remove(duplicate));
    check(q.paths().back() == "last" && q.entries().back().id != duplicate);
    check(!q.remove(current) && !q.remove(q.entries().front().id));
    auto first_next = q.entries()[2].id;
    auto last = q.entries().back().id;
    check(q.move(last, first_next));
    check(q.entries()[2].id == last && q.entries()[2].category == Q::Category::Next);
    check(q.move(last, q.entries().back().id));
    check(q.entries().back().id == last && q.entries().back().category == Q::Category::Queue);
    q.observe(q.paths(), 3); // audio advances, preserving identity and remaining priority entries
    check(q.current()->path == "next-a");
    check(q.entries()[4].category == Q::Category::Next);
    q.clear();
    check(q.paths() == std::vector<std::string>({"history", "current", "urgent", "next-a"}));
    check(q.current()->path == "next-a");
    q.observe({}, Q::none);
    q.add({"a", "b"}, Q::Action::AfterNext);
    check(!q.current() && q.paths().size() == 2);
    q.clear();
    check(q.paths().empty());
    // Continuation: fed one track at a time, after explicit queue items.
    q.observe({"song-a"}, 0);
    q.set_continuation({"song-b", "song-c", "song-d"});
    check(q.continuation_size() == 3 && q.continuation_front() == "song-b");
    check(q.feed() && !q.feed());
    check(q.paths() == std::vector<std::string>({"song-a", "song-b"}));
    q.add({"queued"}, Q::Action::Append);
    q.add({"next"}, Q::Action::PlayNext);
    check(q.paths() == std::vector<std::string>({"song-a", "next", "queued", "song-b"}));
    check(!q.move(q.entries().back().id, q.entries()[1].id) && !q.move(q.entries()[1].id, q.entries().back().id));
    q.observe(q.paths(), 3); // song-b plays; the next one is fed
    check(q.continuation_size() == 2 && q.feed() && q.paths().back() == "song-c");
    check(q.remove_continuation() && q.continuation_size() == 0 && !q.feed());
    check(q.paths() == std::vector<std::string>({"song-a", "next", "queued", "song-b"}));
    q.set_continuation({"x"});
    q.reset({"album"}, 0); // a new context replaces it
    check(q.continuation_size() == 0 && !q.feed());
    q.set_continuation({"y"});
    q.clear();
    check(q.continuation_size() == 0);
    std::cout << "Queue priority, reordering, duplicates, progression, clear and continuation passed\n";
}
