#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/guidance/indexer.hpp"
#include "adapters/models/model_store.hpp"
#include "core/guidance_scan.hpp"

namespace ambient::guidance {
namespace {

constexpr const char* kFixtureDir = AMBIENT_GUIDANCE_FIXTURE_DIR;

const models::ModelStore& Store() {
    static const models::ModelStore store{std::filesystem::path(AMBIENT_MODELS_DIR)};
    return store;
}

Embedder& StagedEmbedder() {
    static const auto embedder = Embedder::Load(Store());
    return *embedder;
}

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0;
    for (std::size_t i = 0; i < a.size(); ++i) dot += static_cast<double>(a[i]) * b[i];
    return dot;
}

// The staged model loads through the guards and matches the Python harness
// on the fixture texts it embedded
TEST(GuidanceEmbedder, LoadsAndMatchesTheHarnessEmbeddings) {
    auto& embedder = StagedEmbedder();
    const auto& identity = embedder.Identity();
    EXPECT_EQ(identity.id, "gte-large-int8");
    EXPECT_EQ(identity.dim, 1024);
    EXPECT_EQ(identity.max_tokens, 512);
    EXPECT_EQ(identity.rev,
              Store().Resolve("embedding", "default").file_hashes.at("openvino_model.bin"));

    std::ifstream in(std::filesystem::path(kFixtureDir) / "embeddings.json");
    ASSERT_TRUE(in.is_open());
    const auto fixture = nlohmann::json::parse(in);
    ASSERT_EQ(fixture.at("model"), "gte-large-int8");
    const auto texts = fixture.at("texts").get<std::vector<std::string>>();
    const auto vectors = fixture.at("vectors").get<std::vector<std::vector<float>>>();
    for (std::size_t i = 0; i < texts.size(); ++i) {
        const auto ours = embedder.Embed(texts[i]);
        ASSERT_EQ(ours.vector.size(), vectors[i].size());
        EXPECT_GE(Cosine(ours.vector, vectors[i]), 0.999) << texts[i];
        EXPECT_NEAR(Cosine(ours.vector, ours.vector), 1.0, 1e-3) << "unit length";
        EXPECT_FALSE(ours.truncated);
        EXPECT_GT(ours.tokens, 5u);
    }
}

TEST(GuidanceEmbedder, ReportsTruncationOnALongText) {
    std::string long_text;
    for (int i = 0; i < 1500; ++i) long_text += "symptom ";
    const auto out = StagedEmbedder().Embed(long_text);
    EXPECT_TRUE(out.truncated);
    EXPECT_GT(out.tokens, 512u);
    EXPECT_EQ(out.vector.size(), 1024u);
}

TEST(GuidanceEmbedder, IndexesTheFixtureCorpusAndFindsTheRightGuideline) {
    const auto dir = std::filesystem::temp_directory_path() / "ambient-index-real";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "docs");
    // one markdown file per fixture guideline, recommendations numbered
    std::ifstream in(std::filesystem::path(kFixtureDir) / "corpus.jsonl");
    std::map<std::string, std::string> per_code;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const auto row = nlohmann::json::parse(line);
        const auto code = row.at("code").get<std::string>();
        auto number = row.at("id").get<std::string>().substr(code.size() + 1);
        std::replace(number.begin(), number.end(), '_', '.');
        per_code[code] += number + " " + row.at("text").get<std::string>() + "\n\n";
    }
    for (const auto& [code, text] : per_code) std::ofstream(dir / "docs" / (code + ".md")) << text;
    std::ofstream(dir / "build.json")
        << R"({"id": "fixture-real", "name": "Fixture", "licence": "invented", "attribution": "none",
               "source": "text", "text": {"dir": "docs"}})";

    auto& embedder = StagedEmbedder();
    const auto report = IndexCorpus(ReadBuildSpec(dir / "build.json"), embedder, dir / "out",
                                    "2026-09-11T00:00:00Z", "models_tests");
    EXPECT_EQ(report.chunks, 40u);
    {
        std::string reason;
        const auto store = CorpusStore::Open(dir / "out", embedder.Identity(), reason);
        ASSERT_NE(store, nullptr) << reason;
        EXPECT_EQ(store->Size(), report.chunks);

        const auto query = embedder.Embed(
            "Urgent rheumatology referral for a woman with synovitis in several small joints of "
            "the hands.");
        const auto hits =
            Scan(store->Matrix(), store->Size(), store->Dim(), query.vector.data(), 3);
        ASSERT_EQ(hits.size(), 3u);
        EXPECT_EQ(store->CiteAt(hits[0].ord).code, "fx100") << store->TextAt(hits[0].ord).text;
        EXPECT_GT(hits[0].cosine, 0.8f);
    }
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace ambient::guidance
