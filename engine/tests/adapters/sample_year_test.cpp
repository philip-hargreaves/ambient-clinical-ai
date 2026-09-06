#include "adapters/demo/sample_year.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <set>

#include "core/summary_scrub.hpp"

namespace ambient::demo {
namespace {

using namespace std::chrono;

// The shipped content: eight consultations over eight distinct months, each
// whole, and nothing in what the app shows that the scrub would change
TEST(SampleYear, TheShippedContentIsWholeAndSpreadOverTheYear) {
    const auto samples = LoadSampleYear(AMBIENT_DEMO_DIR);
    ASSERT_EQ(samples.size(), 8u);
    std::set<int> months;
    std::set<std::string> sources;
    for (const auto& s : samples) {
        months.insert(s.months_back);
        sources.insert(s.source);
        EXPECT_GE(s.months_back, 1) << s.source;
        EXPECT_LE(s.months_back, 12) << s.source;
        EXPECT_FALSE(s.label.empty()) << s.source;
        EXPECT_FALSE(s.note.empty()) << s.source;
        EXPECT_FALSE(s.patient.empty()) << s.source;
        EXPECT_FALSE(s.summary.empty()) << s.source;
        EXPECT_GT(s.turns.size(), 50u) << s.source;
        EXPECT_GT(s.audio_seconds, 300.0) << s.source;
        EXPECT_FALSE(s.happened.empty()) << s.source;
        EXPECT_FALSE(s.learned.empty()) << s.source;
        EXPECT_FALSE(s.next.empty()) << s.source;
        for (const std::string* text : {&s.summary, &s.happened, &s.learned, &s.next}) {
            EXPECT_EQ(core::ScrubSummary(*text), *text) << s.source;
        }
        std::uint64_t last_start = 0;
        for (const auto& turn : s.turns) {
            EXPECT_GE(turn.first_frame, last_start) << s.source;
            EXPECT_TRUE(turn.speaker == "doctor" || turn.speaker == "patient") << s.source;
            last_start = turn.first_frame;
        }
    }
    EXPECT_EQ(months.size(), 8u) << "two samples in one month";
    EXPECT_EQ(sources.size(), 8u) << "a consultation used twice";
}

TEST(SampleYear, StartsFallOnTheSampleDayMonthsBackClampedToTheMonth) {
    Sample sample;
    sample.months_back = 1;
    sample.day = 31;
    sample.hour = 13;
    sample.minute = 15;
    // 15 March 2026 -> February has 28 days, so the 31st becomes the 28th
    const sys_seconds now = sys_days{2026y / March / 15} + hours{10};
    EXPECT_EQ(Iso8601(SampleStart(sample, now)), "2026-02-28T13:15:00Z");

    sample.months_back = 11;
    sample.day = 4;
    EXPECT_EQ(Iso8601(SampleStart(sample, now)), "2025-04-04T13:15:00Z");
}

}  // namespace
}  // namespace ambient::demo
