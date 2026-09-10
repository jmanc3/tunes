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
    std::cout << "Queue priority, reordering, duplicates, progression and clear passed\n";
}
