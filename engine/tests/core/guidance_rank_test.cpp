#include "core/guidance_rank.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::guidance {
namespace {

// Three sub-queries, hand-computed reciprocal rank fusion with k = 60:
//   y: 1/62 + 1/61 + 1/62   x: 1/61 + 1/62   z: 1/63 + 1/61
std::vector<SubQueryHits> Lists() {
    return {{"sentence one", false, {{"x", 0.90}, {"y", 0.88}, {"z", 0.80}}},
            {"sentence two", false, {{"y", 0.91}, {"x", 0.86}}},
            {"the whole note", true, {{"z", 0.87}, {"y", 0.84}}}};
}

TEST(RankVote, ReproducesTheHarnessFusion) {
    const auto ranked = RankVote(Lists());
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].id, "y");
    EXPECT_EQ(ranked[1].id, "x");
    EXPECT_EQ(ranked[2].id, "z");
    EXPECT_NEAR(ranked[0].score, 1.0 / 62 + 1.0 / 61 + 1.0 / 62, 1e-12);
    EXPECT_NEAR(ranked[1].score, 1.0 / 61 + 1.0 / 62, 1e-12);
    EXPECT_NEAR(ranked[2].score, 1.0 / 63 + 1.0 / 61, 1e-12);
}

TEST(RankVote, KeepsTheBestCosineAndTheQueryThatRankedItHighest) {
    const auto ranked = RankVote(Lists());
    EXPECT_DOUBLE_EQ(ranked[0].cosine, 0.91);
    EXPECT_EQ(ranked[0].trigger, "sentence two");
    EXPECT_EQ(ranked[1].trigger, "sentence one");
    EXPECT_EQ(ranked[2].trigger, "the whole note");
}

TEST(RankVote, TheNoteWeightMultipliesEveryVoteOfTheWholeNoteList) {
    // weight 3: y = 1/62 + 1/61 + 3/62, z = 1/63 + 3/61, x = 1/61 + 1/62; z passes x, y stays first
    const auto ranked = RankVote(Lists(), 3);
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].id, "y");
    EXPECT_EQ(ranked[1].id, "z");
    EXPECT_EQ(ranked[2].id, "x");
    EXPECT_NEAR(ranked[1].score, 1.0 / 63 + 3.0 / 61, 1e-12);
    EXPECT_NEAR(ranked[0].score, 1.0 / 62 + 1.0 / 61 + 3.0 / 62, 1e-12);
    EXPECT_EQ(RankVote(Lists(), 0)[2].id, "z") << "a weight under one counts as one";
}

TEST(RankVote, HonoursTheUnionSize) {
    std::vector<SubQueryHits> lists{{"q", false, {}}};
    for (int i = 0; i < 80; ++i) lists[0].hits.push_back({"c" + std::to_string(i), 0.9});
    EXPECT_EQ(RankVote(lists).size(), static_cast<std::size_t>(kUnionSize));
    EXPECT_EQ(RankVote(lists, 1, 10).size(), 10u);
}

TEST(ApplyFloor, DropsUnderTheFloorAndAbstainsWhenNothingRemains) {
    const auto kept = ApplyFloor(RankVote(Lists()), 0.875);
    ASSERT_EQ(kept.kept.size(), 2u) << "z's best cosine is 0.87, under the floor";
    EXPECT_EQ(kept.kept[0].id, "y");
    EXPECT_EQ(kept.kept[1].id, "x");
    EXPECT_EQ(kept.considered, 3);
    EXPECT_FALSE(kept.abstained);
    const auto none = ApplyFloor(RankVote(Lists()), 0.95);
    EXPECT_TRUE(none.abstained);
    EXPECT_TRUE(none.kept.empty());
    EXPECT_EQ(none.considered, 3);
}

TEST(PopulationConflict, PregnancyAgeAndSexOnlyWhenTheNoteIsExplicit) {
    const std::string uti =
        "A 19-year-old woman, not pregnant, reports three days of dysuria. Plan: three-day course.";
    EXPECT_TRUE(PopulationConflict(uti, "Offer an immediate antibiotic to pregnant women."));
    EXPECT_FALSE(
        PopulationConflict(uti, "Consider a back-up prescription for women who are not pregnant."));
    EXPECT_TRUE(PopulationConflict(uti, "When prescribing for a man, choose a seven-day course."))
        << "the note says woman";
    EXPECT_TRUE(PopulationConflict(uti, "Refer children with recurrent infection."));
    EXPECT_FALSE(PopulationConflict("Dysuria for three days. Plan: antibiotics.",
                                    "Offer an immediate antibiotic to pregnant women."))
        << "silence on pregnancy is not a contradiction";
    EXPECT_FALSE(PopulationConflict("A 9-year-old child with fever.",
                                    "Refer children with recurrent infection."));
    EXPECT_TRUE(PopulationConflict("Patient aged 72 with new back pain.",
                                   "Refer children with recurrent infection."));
}

TEST(Citation, CodeSectionTitle) {
    EXPECT_EQ(Citation("fx100", "1.1 Referral", "Fictional inflammatory joint disease"),
              "FX100, 1.1 Referral: Fictional inflammatory joint disease");
    EXPECT_EQ(Citation("ng100", "", ""), "NG100");
}

struct NoRerank : IReranker {
    std::vector<double> Score(const std::string&, const std::vector<std::string>& texts) override {
        return std::vector<double>(texts.size(), 0.0);
    }
};

TEST(IReranker, TheSeamCompilesAndCanBeANoOp) {
    NoRerank none;
    EXPECT_EQ(none.Score("q", {"a", "b"}).size(), 2u);
}

}  // namespace
}  // namespace ambient::guidance
