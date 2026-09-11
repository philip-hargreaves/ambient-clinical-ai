#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/guidance/indexer.hpp"
#include "adapters/guidance/retriever.hpp"
#include "adapters/models/model_store.hpp"
#include "core/guidance_scan.hpp"
#include "guidance_fixture.hpp"

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

// The one loaded model, lent to a retriever that expects to own its embedder
struct Borrowed : IEmbedder {
    IEmbedder& inner;
    explicit Borrowed(IEmbedder& embedder) : inner(embedder) {}
    const EmbedderIdentity& Identity() const override {
        return inner.Identity();
    }
    Embedding Embed(const std::string& text) override {
        return inner.Embed(text);
    }
};

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0;
    for (std::size_t i = 0; i < a.size(); ++i) dot += static_cast<double>(a[i]) * b[i];
    return dot;
}

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
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
    fixture::WriteMarkdown(kFixtureDir, dir / "docs");
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

// The retriever over the staged model and the fixture corpus: each fixture
// note surfaces its guideline first and one of its expected recommendations in
// the top three, never a guideline it must not; the non-clinical text is
// refused at the shipped floor. Ordering is asserted without the floor; what
// the floor does with each note is printed, since the fixture is invented text
TEST(GuidanceEmbedder, SearchesTheFixtureNotes) {
    const auto dir = std::filesystem::temp_directory_path() / "ambient-guidance-real";
    std::filesystem::remove_all(dir);
    auto& embedder = StagedEmbedder();
    fixture::Build(dir / "corpora" / "fixture", "fixture", embedder, fixture::Chunks(kFixtureDir));
    {
        auto lend = [&]() -> std::unique_ptr<IEmbedder> {
            return std::make_unique<Borrowed>(embedder);
        };
        RetrieverOptions unfloored;
        unfloored.floor = 0;
        Retriever ordering(lend, dir / "corpora", unfloored);
        Retriever shipped(lend, dir / "corpora");
        for (const auto& note : fixture::Notes(kFixtureDir)) {
            const auto results = ordering.Search(note.text, 3);
            const auto floored = shipped.Search(note.text, 3);
            std::vector<std::string> ids;
            for (const auto& r : results.shown) ids.push_back(r.chunk_id);
            if (note.expected.empty()) {
                EXPECT_TRUE(floored.abstained) << note.id << ": " << Join(ids);
                continue;
            }
            ASSERT_FALSE(ids.empty()) << note.id;
            const auto code = note.expected[0].substr(0, note.expected[0].find('-'));
            EXPECT_EQ(results.shown[0].guideline, code) << note.id << ": " << Join(ids);
            bool any = false;
            for (const auto& id : note.expected) any |= std::count(ids.begin(), ids.end(), id) > 0;
            EXPECT_TRUE(any) << note.id << ": " << Join(ids);
            for (const auto& banned : note.must_not) {
                for (const auto& r : results.shown) EXPECT_NE(r.guideline, banned) << note.id;
            }
            std::printf("  %s: top cosine %.3f, %zu of %d shown at the shipped floor\n",
                        note.id.c_str(), results.shown[0].score, floored.shown.size(),
                        floored.considered);
        }
    }
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace ambient::guidance
