#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "adapters/guidance/chunker.hpp"
#include "core/env_flag.hpp"

namespace ambient::guidance {
namespace {

nlohmann::json Document() {
    return nlohmann::json::parse(R"json({
        "code": "fx100",
        "title": "Fictional inflammatory joint disease",
        "source_url": "https://example.test/guidance/fx100",
        "last_updated": "2026-01-01",
        "chapters": [
            {"title": "Recommendations", "slug": "Recommendations", "recommendations": [
                {"id": "fx100-1_1_1", "kind": "recommendation", "number": "1.1.1",
                 "section": "1.1 Referral", "update_tag": "2026", "text": "  Refer adults with persistent synovitis.  "},
                {"id": "fx100-1_1_2", "kind": "recommendation", "number": "1.1.2",
                 "section": "1.1 Referral", "text": "Refer urgently if the small joints are affected."},
                {"id": "fx100-p_3", "kind": "numbered_paragraph", "number": "3", "text": "Not a recommendation."}
            ]},
            {"title": "Recommendations (copy)", "slug": "recommendations", "recommendations": [
                {"id": "fx100-1_1_1", "kind": "recommendation", "number": "1.1.1",
                 "section": "1.1 Referral", "text": "Refer adults with persistent synovitis."},
                {"id": "fx100-1_2_1", "kind": "recommendation", "number": "1.2.1",
                 "section": "1.2 Investigations", "text": "Offer a blood test for rheumatoid factor."}
            ]}
        ]
})json");
}

TEST(ChunksFromDocument, OneChunkPerRecommendationWithDuplicatesAndParagraphsSkipped) {
    std::set<std::string> seen;
    const auto chunks = ChunksFromDocument(Document(), seen);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].id, "fx100-1_1_1");
    EXPECT_EQ(chunks[0].text, "Refer adults with persistent synovitis.") << "trimmed";
    EXPECT_EQ(chunks[0].section, "1.1 Referral");
    EXPECT_EQ(chunks[0].number, "1.1.1");
    EXPECT_EQ(chunks[0].update_tag, "2026");
    EXPECT_EQ(chunks[0].last_updated, "2026-01-01");
    EXPECT_EQ(chunks[0].url,
              "https://example.test/guidance/fx100/chapter/Recommendations#fx100-1_1_1");
    EXPECT_EQ(chunks[0].source, "nice");
    EXPECT_EQ(chunks[1].id, "fx100-1_1_2");
    EXPECT_EQ(chunks[2].id, "fx100-1_2_1") << "the copied chapter contributes only its new id";
    EXPECT_EQ(chunks[2].chapter, "Recommendations (copy)");
    EXPECT_EQ(seen.size(), 3u);
}

TEST(IncludeDocument, RequestedCurrentAndNotAStub) {
    const std::set<std::string> requested{"fx100", "fx200"};
    EXPECT_TRUE(IncludeDocument({{"status", "current"}, {"is_stub", false}}, "fx100", requested));
    EXPECT_FALSE(IncludeDocument({{"status", "current"}}, "fx300", requested)) << "not requested";
    EXPECT_FALSE(IncludeDocument({{"status", "current"}, {"is_stub", true}}, "fx100", requested));
    EXPECT_FALSE(IncludeDocument({{"status", "withdrawn"}}, "fx200", requested));
    EXPECT_FALSE(IncludeDocument(nlohmann::json::object(), "fx200", requested)) << "no status";
}

TEST(StartsRecommendation, NumberedOpeningsOnly) {
    EXPECT_TRUE(StartsRecommendation("Recommendation 12"));
    EXPECT_TRUE(StartsRecommendation("recommendation 3a Consider a blood test"));
    EXPECT_TRUE(StartsRecommendation("1.2 Investigations"));
    EXPECT_TRUE(StartsRecommendation("1.2.3 Offer first-line treatment"));
    EXPECT_FALSE(StartsRecommendation("1 Introduction"));
    EXPECT_FALSE(StartsRecommendation("Version 1.2 of the guideline"));
    EXPECT_FALSE(StartsRecommendation("10 mg twice a day"));
    EXPECT_FALSE(StartsRecommendation("Recommendations for research"));
    EXPECT_FALSE(StartsRecommendation("1.2.3mg is the dose"));
}

std::string Paragraph(int words) {
    std::string out;
    for (int i = 0; i < words; ++i) out += (i ? " word" : "word");
    return out;
}

TEST(ChunksFromText, RunsCloseAtHeadingsAndAtTheTargetLength) {
    const std::string text = "A short title\n\n" + Paragraph(20) + "\n\n" + Paragraph(20) +
                             "\n\n1.1.1 " + Paragraph(30) + "\n \n" + Paragraph(280) + "\n\n" +
                             Paragraph(40) + "\n\nRecommendation 2 " + Paragraph(10);
    const auto chunks = ChunksFromText("doc", "A document", text, "doc.md");
    ASSERT_EQ(chunks.size(), 4u) << chunks.size();
    EXPECT_EQ(chunks[0].id, "doc-1");
    EXPECT_EQ(chunks[0].text, Paragraph(20) + " " + Paragraph(20)) << "the 3-word title is dropped";
    EXPECT_EQ(chunks[0].number, "");
    EXPECT_EQ(chunks[1].number, "1.1.1") << "a heading opens a run and names it";
    EXPECT_EQ(chunks[1].text.substr(0, 6), "1.1.1 ");
    EXPECT_EQ(chunks[2].text, Paragraph(40)) << "the run before it reached the target length";
    EXPECT_EQ(chunks[3].text.substr(0, 16), "Recommendation 2");
    EXPECT_EQ(chunks[3].url, "doc.md");
    EXPECT_EQ(chunks[3].source, "text");
    EXPECT_TRUE(ChunksFromText("x", "x", "too short\n\nalso short").empty());
}

TEST(ChunksFromText, NeverExceedsTheMaximum) {
    std::string text;
    for (int i = 0; i < 10; ++i) text += Paragraph(250) + "\n\n";
    for (const auto& chunk : ChunksFromText("doc", "A document", text)) {
        int words = 1;
        for (char c : chunk.text) words += c == ' ';
        EXPECT_LE(words, kMaxWords);
    }
}

// The engine chunker over the real NICE research copy must reproduce the
// harness's chunk file exactly. Opt in with the two paths; skipped elsewhere
TEST(ChunksFromDocument, ReproducesTheHarnessChunkFileWhenTheCorpusIsPresent) {
    const auto dir = EnvValue("AMBIENT_NICE_DIR");
    const auto file = EnvValue("AMBIENT_NICE_CHUNKS");
    if (dir.empty() || file.empty()) GTEST_SKIP() << "set AMBIENT_NICE_DIR and AMBIENT_NICE_CHUNKS";
    const std::filesystem::path root(dir);
    std::set<std::string> requested;
    {
        std::ifstream codes(root.parent_path() / "codes.txt");
        for (std::string line; std::getline(codes, line);) {
            const auto code = line.substr(0, line.find('#'));
            const auto end = code.find_last_not_of(" \t\r");
            if (end != std::string::npos) {
                std::string c = code.substr(0, end + 1);
                for (auto& ch : c)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                requested.insert(c);
            }
        }
    }
    std::ifstream manifest_in(root / "manifest.json");
    const auto manifest = nlohmann::json::parse(manifest_in);
    std::map<std::string, Chunk> ours;
    std::set<std::string> seen;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(root / "json"))
        files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream in(path);
        const auto doc = nlohmann::json::parse(in);
        const auto code = doc.at("code").get<std::string>();
        if (!IncludeDocument(manifest.value(code, nlohmann::json::object()), code, requested))
            continue;
        for (auto& chunk : ChunksFromDocument(doc, seen)) ours.emplace(chunk.id, std::move(chunk));
    }
    std::ifstream harness(file);
    std::size_t rows = 0, mismatched = 0;
    for (std::string line; std::getline(harness, line);) {
        if (line.empty()) continue;
        ++rows;
        const auto row = nlohmann::json::parse(line);
        const auto it = ours.find(row.at("id").get<std::string>());
        const auto section =
            row.at("section").is_string() ? row.at("section").get<std::string>() : std::string();
        if (it == ours.end() || it->second.text != row.at("text").get<std::string>() ||
            it->second.url != row.at("url").get<std::string>() || it->second.section != section) {
            ++mismatched;
        }
    }
    EXPECT_EQ(ours.size(), rows);
    EXPECT_EQ(mismatched, 0u);
}

}  // namespace
}  // namespace ambient::guidance
