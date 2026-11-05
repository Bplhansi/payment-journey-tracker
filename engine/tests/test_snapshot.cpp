// Step 10: surviving a crash. Save a snapshot, "crash", reload, replay from the saved
// position, and check the final result matches a run that never crashed.
#include <gtest/gtest.h>

#include <filesystem>
#include <random>

#include "helpers.hpp"
#include "pjt/util.hpp"

using namespace pjt;
using namespace pjt::testing;

namespace {

std::vector<Event> messy_stream() {
    std::vector<Event> s;
    std::mt19937 rng(7);
    for (int i = 0; i < 400; ++i) {
        auto p = healthy("pay-" + std::to_string(i), i * 30, 120);
        if (i % 9 == 0) p.resize(2);                       // stuck after Validated
        else if (i % 10 == 0) p.back().stage = Stage::Failed;
        else if (i % 17 == 0) p.erase(p.begin() + 1);      // skipped stage
        if (i % 5 == 0) p.push_back(p[1 % p.size()]);      // a duplicate
        s.insert(s.end(), p.begin(), p.end());
    }
    std::stable_sort(s.begin(), s.end(), [](const Event& a, const Event& b) { return a.timestamp_ms < b.timestamp_ms; });
    for (std::size_t i = 0; i + 1 < s.size(); i += 7) std::swap(s[i], s[i + 1]);  // some out of order
    return s;
}

}  // namespace

TEST(Snapshot, SaveAndRestoreGivesIdenticalState) {
    const auto stream = messy_stream();
    CollectSink sink_a;
    Tracker a(test_config(), sink_a);
    for (std::size_t i = 0; i < stream.size() / 2; ++i) a.process(stream[i]);
    const auto snap = a.snapshot_json("123-0");

    CollectSink sink_b;
    Tracker b(test_config(), sink_b);
    EXPECT_EQ(b.restore_json(snap), "123-0");
    EXPECT_EQ(b.snapshot_json("123-0"), snap);  // nothing lost or changed
    EXPECT_EQ(b.now(), a.now());
    EXPECT_EQ(b.payments().size(), a.payments().size());
}

TEST(Snapshot, CrashMidRunMatchesCleanRun) {
    const auto stream = messy_stream();

    CollectSink clean_sink;
    {
        Tracker t(test_config(), clean_sink);
        for (const auto& e : stream) t.process(e);
        t.drain();
    }

    for (std::size_t crash_at : {std::size_t{50}, stream.size() / 3, stream.size() / 2, stream.size() - 5}) {
        // Everything the engine ever reported goes to one sink, like the output stream would.
        CollectSink out;
        const std::size_t snapshot_at = crash_at - 37;  // the last snapshot was a little earlier
        std::string snap;
        {
            Tracker t(test_config(), out);
            for (std::size_t i = 0; i < crash_at; ++i) {
                t.process(stream[i]);
                if (i + 1 == snapshot_at) snap = t.snapshot_json(std::to_string(snapshot_at));
            }
            // crash: the tracker is destroyed without saving anything else
        }
        Tracker r(test_config(), out);
        const auto from = std::stoull(r.restore_json(snap));
        for (std::size_t i = from; i < stream.size(); ++i) r.process(stream[i]);  // replays 37 events twice
        r.drain();

        const auto clean = outcome_of(clean_sink);
        const auto got = outcome_of(out);
        EXPECT_EQ(std::get<0>(got), std::get<0>(clean)) << "crash at " << crash_at;
        EXPECT_EQ(std::get<1>(got), std::get<1>(clean)) << "crash at " << crash_at;
    }
}

TEST(Snapshot, DuplicatesAfterRestartAreStillIgnored) {
    CollectSink sink;
    Tracker a(test_config(), sink);
    auto e = healthy("p", 0);
    a.process(e[0]);
    a.process(e[1]);
    Tracker b(test_config(), sink);
    b.restore_json(a.snapshot_json("x"));
    b.process(e[1]);  // replayed after the restart
    EXPECT_EQ(b.stats().duplicates, 1);
    EXPECT_EQ(b.payments().at("p").history.size(), 2u);
}

TEST(Snapshot, FileIsWrittenAtomically) {
    const auto dir = std::filesystem::temp_directory_path() / "pjt_snapshot_test";
    std::filesystem::remove_all(dir);
    const auto path = (dir / "engine.snapshot").string();
    write_file_atomic(path, "first");
    write_file_atomic(path, "second");  // replaces the old one
    std::string text;
    ASSERT_TRUE(read_file(path, text));
    EXPECT_EQ(text, "second");
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove_all(dir);
}

TEST(Snapshot, RejectsUnknownVersion) {
    CollectSink sink;
    Tracker t(test_config(), sink);
    EXPECT_ANY_THROW(t.restore_json(R"({"version": 99})"));
}
