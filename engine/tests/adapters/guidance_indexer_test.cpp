#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/indexer.hpp"

namespace ambient::guidance {
namespace {

constexpr const char* kFixtureDir = AMBIENT_GUIDANCE_FIXTURE_DIR;
constexpr int kDim = 8;

// Deterministic unit vectors from the text, so a rebuild reproduces a corpus
struct FakeEmbedder : IEmbedder {
    EmbedderIdentity identity{"fx-embed-int8", "abc123", kDim, 512, ""};
    const EmbedderIdentity& Identity() const override {
        return identity;
    }
    Embedding Embed(const std::string& text) override {
        std::uint32_t state = 2166136261u;
        for (unsigned char c : text) state = (state ^ c) * 16777619u;
        Embedding out;
        out.tokens = text.size() / 4;
        out.truncated = out.tokens > 512;
        double norm = 0;
        for (int d = 0; d < kDim; ++d) {
            state = state * 1664525u + 1013904223u;
            const float v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            out.vector.push_back(v);
            norm += static_cast<double>(v) * v;
        }
        for (auto& v : out.vector) v /= static_cast<float>(std::sqrt(norm));
        return out;
    }
};

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name)
        : path(std::filesystem::temp_directory_path() / ("ambient-index-" + std::string(name))) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string Paragraph(int words) {
    std::string out;
    for (int i = 0; i < words; ++i) out += (i ? " word" : "word");
    return out;
}

// The fixture corpus as one markdown file per guideline, each recommendation
// numbered so the text chunker keeps them apart
void WriteFixtureAsText(const std::filesystem::path& docs) {
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
    for (const auto& [code, text] : per_code) WriteText(docs / (code + ".md"), text);
}

TEST(ReadCodes, LowerCasesAndDropsComments) {
    TempDir dir("codes");
    WriteText(dir.path / "codes.txt",
              "NG100 # rheumatoid arthritis\nng65\n\n  QS33  \n# only a comment\n");
    const auto codes = ReadCodes(dir.path / "codes.txt");
    EXPECT_EQ(codes, (std::set<std::string>{"ng100", "ng65", "qs33"}));
}

TEST(ChunksFromTextDir, OneDocumentPerFileWithTheHeadingAsTitle) {
    TempDir dir("text");
    WriteText(dir.path / "Gout.md",
              "# Fictional gout guideline\n\n1.1.1 " + Paragraph(20) + "\n\n" + Paragraph(20));
    WriteText(dir.path / "notes.txt", Paragraph(30) + "\n\n" + Paragraph(30));
    WriteText(dir.path / "ignored.pdf", "%PDF");
    const auto chunks = ChunksFromTextDir(dir.path);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].id, "gout-1");
    EXPECT_EQ(chunks[0].title, "Fictional gout guideline");
    EXPECT_EQ(chunks[0].number, "1.1.1");
    EXPECT_EQ(chunks[0].url, "Gout.md");
    EXPECT_EQ(chunks[1].id, "notes-1");
    EXPECT_EQ(chunks[1].title, "notes") << "no heading: the stem";
}

TEST(ChunksFromNiceDir, AppliesTheManifestFilterAcrossDocuments) {
    TempDir dir("nice");
    const nlohmann::json manifest{{"fx100", {{"status", "current"}, {"is_stub", false}}},
                                  {"fx200", {{"status", "current"}, {"is_stub", false}}},
                                  {"fx300", {{"status", "withdrawn"}}}};
    WriteText(dir.path / "manifest.json", manifest.dump());
    auto doc = [](const char* code, const char* rec_id) {
        const nlohmann::json rec{
            {"id", rec_id}, {"kind", "recommendation"}, {"text", "Offer something."}};
        const nlohmann::json chapter{{"title", "Recommendations"},
                                     {"slug", "rec"},
                                     {"recommendations", nlohmann::json::array({rec})}};
        return nlohmann::json{{"code", code},
                              {"title", "Fictional"},
                              {"source_url", "https://example.test"},
                              {"chapters", nlohmann::json::array({chapter})}};
    };
    WriteText(dir.path / "json" / "fx100.json", doc("fx100", "fx100-1_1_1").dump());
    WriteText(dir.path / "json" / "fx200.json", doc("fx200", "fx200-1_1_1").dump());
    WriteText(dir.path / "json" / "fx300.json", doc("fx300", "fx300-1_1_1").dump());
    const auto chunks = ChunksFromNiceDir(dir.path, {"fx100", "fx300"});
    ASSERT_EQ(chunks.size(), 1u) << "fx200 not requested, fx300 withdrawn";
    EXPECT_EQ(chunks[0].id, "fx100-1_1_1");
}

TEST(ReadBuildSpec, ResolvesPathsBesideTheSpec) {
    TempDir dir("spec");
    WriteText(
        dir.path / "build.json",
        R"({"id": "fx-2026-09", "name": "Fixture", "licence": "invented", "attribution": "none",
                  "source": "text", "text": {"dir": "docs"}})");
    const auto spec = ReadBuildSpec(dir.path / "build.json");
    EXPECT_EQ(spec.corpus.id, "fx-2026-09");
    EXPECT_EQ(spec.corpus.source, "text");
    EXPECT_EQ(spec.text_dir, dir.path / "docs");
    WriteText(dir.path / "bad.json",
              R"({"id": "x", "name": "x", "licence": "x", "attribution": "x", "source": "pdf"})");
    EXPECT_THROW(ReadBuildSpec(dir.path / "bad.json"), std::runtime_error);
}

TEST(IndexCorpus, BuildsACorpusTheStoreOpensWithTheEmbeddersIdentity) {
    TempDir dir("build");
    WriteFixtureAsText(dir.path / "docs");
    WriteText(
        dir.path / "build.json",
        R"({"id": "fixture-2026-09", "name": "Fixture", "licence": "invented", "attribution": "none",
                  "source": "text", "text": {"dir": "docs"}})");
    FakeEmbedder embedder;
    std::size_t calls = 0, last_total = 0;
    const auto report =
        IndexCorpus(ReadBuildSpec(dir.path / "build.json"), embedder, dir.path / "out",
                    "2026-09-11T00:00:00Z", "engine_tests", [&](std::size_t, std::size_t total) {
                        ++calls;
                        last_total = total;
                    });
    EXPECT_EQ(report.chunks, 40u) << "one chunk per numbered recommendation";
    EXPECT_EQ(calls, report.chunks);
    EXPECT_EQ(last_total, report.chunks);
    EXPECT_EQ(report.truncated, 0u);

    std::string reason;
    const auto store = CorpusStore::Open(dir.path / "out", embedder.Identity(), reason);
    ASSERT_NE(store, nullptr) << reason;
    EXPECT_EQ(store->Size(), report.chunks);
    EXPECT_EQ(store->Info().embedder_id, "fx-embed-int8");
    EXPECT_EQ(store->Info().built_at, "2026-09-11T00:00:00Z");
    const auto first = embedder.Embed(store->TextAt(0).text).vector;
    for (int d = 0; d < kDim; ++d) EXPECT_FLOAT_EQ(store->Matrix()[d], first[d]);
    auto other = embedder.Identity();
    other.rev = "different";
    EXPECT_EQ(CorpusStore::Open(dir.path / "out", other, reason), nullptr);
}

}  // namespace
}  // namespace ambient::guidance
