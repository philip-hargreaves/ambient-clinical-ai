#include "core/summary_scrub.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace ambient::core {
namespace {

using nlohmann::json;

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(AMBIENT_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

// The same rows the shell's identifier check must pass clean
TEST(SummaryScrub, MatchesTheSharedFixture) {
    for (const auto& row : LoadFixture("summary-scrub.json")) {
        EXPECT_EQ(ScrubSummary(row["in"].get<std::string>()), row["out"].get<std::string>())
            << row["in"].get<std::string>();
    }
}

TEST(SummaryScrub, EveryDecadeHasAName) {
    EXPECT_EQ(ScrubSummary("aged 3"), "under ten");
    EXPECT_EQ(ScrubSummary("aged 14"), "in their teens");
    EXPECT_EQ(ScrubSummary("aged 20"), "in their twenties");
    EXPECT_EQ(ScrubSummary("a 99-year-old"), "a patient in their nineties");
    EXPECT_EQ(ScrubSummary("aged 104"), "in their nineties");
}

TEST(SummaryScrub, AgeSpellingsAllScrub) {
    EXPECT_EQ(ScrubSummary("a 45 yo man"), "a man in their forties");
    EXPECT_EQ(ScrubSummary("a 45 y/o"), "a patient in their forties");
    EXPECT_EQ(ScrubSummary("45 years old"), "in their forties");
    EXPECT_EQ(ScrubSummary("a 45-yr-old woman"), "a woman in their forties");
}

TEST(SummaryScrub, LeavesCleanTextAlone) {
    const std::string text =
        "A patient in their forties presented with a week of painless swelling over one elbow. "
        "Olecranon bursitis was suspected; rest and an anti-inflammatory were agreed.";
    EXPECT_EQ(ScrubSummary(text), text);
    EXPECT_EQ(ScrubSummary(""), "");
}

}  // namespace
}  // namespace ambient::core
