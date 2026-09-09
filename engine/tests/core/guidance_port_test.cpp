#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ports/guidance_retriever.hpp"

namespace ambient::guidance {
namespace {

constexpr const char* kFixtureDir = AMBIENT_GUIDANCE_FIXTURE_DIR;

std::vector<nlohmann::json> ReadLines(const std::string& name) {
    std::ifstream in(std::filesystem::path(kFixtureDir) / name);
    if (!in.is_open()) throw std::runtime_error("missing guidance fixture: " + name);
    std::vector<nlohmann::json> rows;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) rows.push_back(nlohmann::json::parse(line));
    }
    return rows;
}

// Canned retriever over the fixture: a note's expected ids come back in
// fixture order, so tests above the port run without a model
struct FakeGuidanceRetriever : IGuidanceRetriever {
    std::vector<nlohmann::json> corpus = ReadLines("corpus.jsonl");
    std::vector<nlohmann::json> notes = ReadLines("notes.jsonl");

    Results Search(const std::string& note, int limit) override {
        Results results;
        results.considered = static_cast<int>(corpus.size());
        for (const auto& n : notes) {
            if (n.at("text") != note) continue;
            const auto expected = n.at("expected").get<std::vector<std::string>>();
            results.abstained = expected.empty();
            for (const auto& chunk : corpus) {
                if (static_cast<int>(results.shown.size()) >= limit) break;
                const auto id = chunk.at("id").get<std::string>();
                if (std::find(expected.begin(), expected.end(), id) == expected.end()) continue;
                results.shown.push_back({"fixture", id, chunk.at("code"), chunk.at("title"),
                                         chunk.at("section"), chunk.at("text"), 1.0, note});
            }
        }
        return results;
    }

    std::vector<Corpus> Corpora() override {
        return {{"fixture", "Fixture guidance corpus", "invented", static_cast<int>(corpus.size()),
                 "2026-09-09"}};
    }
};

TEST(GuidanceFixture, IdsAreUniqueAndCarryTheirGuidelineCode) {
    const auto corpus = ReadLines("corpus.jsonl");
    ASSERT_GE(corpus.size(), 40u);
    const std::regex shape("fx[0-9]{3}-[0-9]+(_[0-9]+)+");
    std::set<std::string> ids;
    for (const auto& chunk : corpus) {
        const auto id = chunk.at("id").get<std::string>();
        EXPECT_TRUE(std::regex_match(id, shape)) << id;
        EXPECT_EQ(id.substr(0, id.find('-')), chunk.at("code").get<std::string>()) << id;
        EXPECT_TRUE(ids.insert(id).second) << "duplicate " << id;
        EXPECT_FALSE(chunk.at("text").get<std::string>().empty()) << id;
    }
}

TEST(GuidanceFixture, EveryExpectedIdExistsAndTheHardCasesArePresent) {
    std::set<std::string> ids;
    for (const auto& chunk : ReadLines("corpus.jsonl")) ids.insert(chunk.at("id"));
    bool negated = false, non_clinical = false;
    for (const auto& note : ReadLines("notes.jsonl")) {
        for (const auto& id : note.at("expected")) EXPECT_TRUE(ids.count(id)) << id;
        negated |= !note.at("must_not").empty();
        non_clinical |= note.at("expected").empty();
    }
    EXPECT_TRUE(negated) << "a note with a negated finding";
    EXPECT_TRUE(non_clinical) << "a non-clinical text that retrieves nothing";
}

TEST(FakeGuidanceRetriever, HonoursTheLimitAndNamesTheTrigger) {
    FakeGuidanceRetriever fake;
    const auto note = fake.notes.front().at("text").get<std::string>();
    const auto all = fake.Search(note, 10);
    ASSERT_EQ(all.shown.size(), fake.notes.front().at("expected").size());
    EXPECT_FALSE(all.abstained);
    EXPECT_EQ(all.considered, static_cast<int>(fake.corpus.size()));
    EXPECT_EQ(all.shown.front().trigger, note);
    EXPECT_EQ(all.shown.front().guideline, "fx100");
    EXPECT_EQ(fake.Search(note, 1).shown.size(), 1u);
}

TEST(FakeGuidanceRetriever, AbstainsOnNonClinicalText) {
    FakeGuidanceRetriever fake;
    const auto results = fake.Search(fake.notes.back().at("text"), 3);
    EXPECT_TRUE(results.abstained);
    EXPECT_TRUE(results.shown.empty());
    ASSERT_EQ(fake.Corpora().size(), 1u);
    EXPECT_EQ(fake.Corpora().front().chunks, static_cast<int>(fake.corpus.size()));
}

TEST(GuidancePort, DocumentCallsAreNotSupportedYet) {
    FakeGuidanceRetriever fake;
    EXPECT_THROW(fake.AddDocument("C:/somewhere/guideline.pdf"), std::logic_error);
    EXPECT_THROW(fake.RemoveDocument("uploads"), std::logic_error);
}

}  // namespace
}  // namespace ambient::guidance
