#include "core/guidance_query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::guidance {
namespace {

TEST(SplitSentences, MirrorsTheHarnessBoundaries) {
    const auto parts = SplitSentences(
        "A 42-year-old woman presents with six weeks of pain. Examination shows synovitis of "
        "several MCP joints. Bloods show a normal CRP.\nPlan: urgent referral to rheumatology.");
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0], "A 42-year-old woman presents with six weeks of pain.");
    EXPECT_EQ(parts[2], "Bloods show a normal CRP.");
    EXPECT_EQ(parts[3], "Plan: urgent referral to rheumatology.");
}

TEST(SplitSentences, AbbreviationsAndInitialsDoNotEndASentence) {
    const auto parts = SplitSentences(
        "Seen by Dr. Patel with the pt. today. Takes 5 mg. Bd for pain, e.g. After meals. "
        "Reviewed by J. Smith yesterday.");
    ASSERT_EQ(parts.size(), 3u) << parts.size();
    EXPECT_EQ(parts[0], "Seen by Dr. Patel with the pt. today.");
    EXPECT_EQ(parts[1], "Takes 5 mg. Bd for pain, e.g. After meals.");
}

TEST(SplitSentences, LowercaseContinuationsAndFragmentsAreKeptTogetherOrDropped) {
    const auto parts =
        SplitSentences("Pain for 3 days. then eased. OK.\nNil else. Plan as above today.");
    ASSERT_EQ(parts.size(), 2u) << parts.size();
    EXPECT_EQ(parts[0], "Pain for 3 days. then eased.")
        << "a lowercase word continues the sentence";
    EXPECT_EQ(parts[1], "Plan as above today.") << "OK. and Nil else. are under three words";
}

TEST(IsExcluded, NegationsFamilyHistoryAndHypotheticalsAreOut) {
    EXPECT_TRUE(IsExcluded("No headache, no visual disturbance, no neurological symptoms."));
    EXPECT_TRUE(IsExcluded("Nil frequency."));
    EXPECT_TRUE(IsExcluded("Denies chest pain or shortness of breath."));
    EXPECT_TRUE(IsExcluded("FH: mother had type 2 diabetes."));
    EXPECT_TRUE(IsExcluded("Her mother had breast cancer at 52."));
    EXPECT_TRUE(IsExcluded("If symptoms worsen she should attend the emergency department."));
    EXPECT_TRUE(IsExcluded("Not pregnant."));
}

TEST(IsExcluded, FindingsAndPlansStayIn) {
    EXPECT_FALSE(IsExcluded("Presents with a two-day history of dysuria and frequency."));
    EXPECT_FALSE(IsExcluded("Plan: back-up antibiotic prescription and self-care advice."));
    EXPECT_FALSE(IsExcluded("Examination shows synovitis of several MCP joints."));
    EXPECT_FALSE(IsExcluded("Known hypertension, on amlodipine."));
}

TEST(SubQueries, FilteredSentencesThenTheWholeNote) {
    const std::string note =
        "No headache, no visual disturbance, no neurological symptoms. Presents with a two-day "
        "history of dysuria and frequency. Not pregnant. Plan: back-up antibiotic prescription and "
        "self-care advice.";
    const auto queries = SubQueries(note);
    ASSERT_EQ(queries.size(), 3u);
    EXPECT_EQ(queries[0], "Presents with a two-day history of dysuria and frequency.");
    EXPECT_EQ(queries[1], "Plan: back-up antibiotic prescription and self-care advice.");
    EXPECT_EQ(queries[2], note) << "the whole note is the last sub-query";
}

TEST(SubQueries, AOneSentenceNoteIsOneQuery) {
    const auto queries = SubQueries("Presents with a two-day history of dysuria.");
    ASSERT_EQ(queries.size(), 1u);
    EXPECT_TRUE(SubQueries("Minutes.").empty());
}

}  // namespace
}  // namespace ambient::guidance
