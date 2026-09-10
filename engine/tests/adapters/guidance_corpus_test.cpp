#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/guidance/chunker.hpp"
#include "adapters/guidance/corpus_builder.hpp"
#include "adapters/guidance/corpus_store.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/storage/db.hpp"

namespace ambient::guidance {
namespace {

constexpr const char* kFixtureDir = AMBIENT_GUIDANCE_FIXTURE_DIR;
constexpr int kDim = 8;

std::vector<Chunk> FixtureChunks() {
    std::ifstream in(std::filesystem::path(kFixtureDir) / "corpus.jsonl");
    if (!in.is_open()) throw std::runtime_error("missing guidance fixture corpus");
    std::vector<Chunk> chunks;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const auto row = nlohmann::json::parse(line);
        Chunk c;
        c.id = row.at("id");
        c.code = row.at("code");
        c.title = row.at("title");
        c.section = row.at("section");
        c.text = row.at("text");
        c.url = "https://example.test/" + c.id;
        c.source = "text";
        chunks.push_back(std::move(c));
    }
    return chunks;
}

// Deterministic unit vectors, one per chunk
std::vector<float> FixtureVectors(std::size_t rows) {
    std::vector<float> out(rows * kDim);
    std::uint32_t state = 12345;
    for (std::size_t r = 0; r < rows; ++r) {
        double norm = 0;
        for (int d = 0; d < kDim; ++d) {
            state = state * 1664525u + 1013904223u;
            const float v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            out[r * kDim + d] = v;
            norm += double(v) * v;
        }
        for (int d = 0; d < kDim; ++d) out[r * kDim + d] /= static_cast<float>(std::sqrt(norm));
    }
    return out;
}

EmbedderIdentity Embedder() {
    return {"fx-embed-int8", "abc123", kDim, 512, ""};
}

CorpusSpec Spec() {
    CorpusSpec spec;
    spec.id = "fixture-2026-09-10";
    spec.name = "Fixture guidance corpus";
    spec.licence = "invented";
    spec.attribution = "none";
    spec.source = "text";
    spec.embedder_id = "fx-embed-int8";
    spec.embedder_rev = "abc123";
    spec.dim = kDim;
    spec.built_at = "2026-09-10T00:00:00Z";
    spec.builder = "engine_tests";
    return spec;
}

struct CorpusDir {
    std::filesystem::path path;
    explicit CorpusDir(const char* name)
        : path(std::filesystem::temp_directory_path() / ("ambient-corpus-" + std::string(name))) {
        std::filesystem::remove_all(path);
    }
    ~CorpusDir() {
        std::error_code ignored;
        for (const auto& e : std::filesystem::recursive_directory_iterator(path, ignored)) {
            SetFileAttributesW(e.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        std::filesystem::remove_all(path, ignored);
    }
};

void Build(const CorpusDir& dir) {
    const auto chunks = FixtureChunks();
    BuildCorpus(dir.path, Spec(), chunks, FixtureVectors(chunks.size()));
}

// After editing corpus.db by hand, make the manifest agree again so a deeper guard is reached
void Rehash(const CorpusDir& dir) {
    const auto manifest_path = dir.path / kManifestFile;
    std::ifstream in(manifest_path);
    auto manifest = nlohmann::json::parse(in);
    in.close();
    const auto db = dir.path / kCorpusFile;
    manifest["bytes"] = std::filesystem::file_size(db);
    manifest["sha256"] = models::Sha256File(db);
    std::ofstream out(manifest_path, std::ios::trunc);
    out << manifest.dump(2);
}

void EditManifest(const CorpusDir& dir, const char* key, const nlohmann::json& value) {
    const auto manifest_path = dir.path / kManifestFile;
    std::ifstream in(manifest_path);
    auto manifest = nlohmann::json::parse(in);
    in.close();
    manifest[key] = value;
    std::ofstream out(manifest_path, std::ios::trunc);
    out << manifest.dump(2);
}

void Sql(const CorpusDir& dir, const char* sql) {
    store::Db db(dir.path / kCorpusFile, store::Db::Mode::kBuild);
    db.Exec(sql);
}

std::string Refusal(const CorpusDir& dir) {
    std::string reason;
    const auto store = CorpusStore::Open(dir.path, Embedder(), reason);
    EXPECT_EQ(store, nullptr);
    return reason;
}

TEST(CorpusStore, BuildsOpensAndFillsTheMatrixInOrdinalOrder) {
    CorpusDir dir("build");
    Build(dir);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "corpus.db.tmp"));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "corpus.db-wal"));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "corpus.db-shm"));

    std::string reason;
    const auto store = CorpusStore::Open(dir.path, Embedder(), reason);
    ASSERT_NE(store, nullptr) << reason;
    EXPECT_TRUE(reason.empty());
    const auto chunks = FixtureChunks();
    const auto vectors = FixtureVectors(chunks.size());
    ASSERT_EQ(store->Size(), chunks.size());
    EXPECT_EQ(store->Dim(), kDim);
    for (std::size_t i = 0; i < vectors.size(); ++i) EXPECT_EQ(store->Matrix()[i], vectors[i]);
    EXPECT_EQ(store->CiteAt(0).chunk_id, chunks[0].id);
    EXPECT_EQ(store->CiteAt(0).section, chunks[0].section);
    EXPECT_EQ(store->CiteAt(chunks.size() - 1).chunk_id, chunks.back().id);
    EXPECT_EQ(store->TextAt(3).text, chunks[3].text);
    EXPECT_EQ(store->TextAt(3).url, chunks[3].url);
    EXPECT_EQ(store->Info().id, "fixture-2026-09-10");
    EXPECT_EQ(store->Info().chunk_count, static_cast<std::int64_t>(chunks.size()));
    EXPECT_EQ(store->Info().sha256, models::Sha256File(dir.path / kCorpusFile));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "corpus.db-wal"))
        << "read-only open writes nothing";
}

TEST(CorpusStore, TheFileUsesARollbackJournalAndTheCorpusPageSize) {
    CorpusDir dir("journal");
    Build(dir);
    std::ifstream in(dir.path / kCorpusFile, std::ios::binary);
    unsigned char header[100];
    in.read(reinterpret_cast<char*>(header), sizeof header);
    EXPECT_EQ(header[18], 1) << "file format 1: rollback journal, not WAL";
    EXPECT_EQ(header[19], 1);
    EXPECT_EQ((header[16] << 8) | header[17], 1) << "page size 65536 is stored as 1";
}

TEST(CorpusStore, RefusesAHashThatDoesNotMatchTheManifest) {
    CorpusDir dir("hash");
    Build(dir);
    EditManifest(dir, "sha256", std::string(64, '0'));
    EXPECT_NE(Refusal(dir).find("hash"), std::string::npos);
}

TEST(CorpusStore, RefusesWhenMetaAndManifestDisagree) {
    for (const char* key : {"id", "dim", "embedder_id", "chunks"}) {
        CorpusDir dir("meta");
        Build(dir);
        if (std::string(key) == "dim") {
            EditManifest(dir, key, kDim + 1);
        } else if (std::string(key) == "chunks") {
            EditManifest(dir, key, 3);
        } else {
            EditManifest(dir, key, "other");
        }
        const auto reason = Refusal(dir);
        EXPECT_FALSE(reason.empty()) << key;
        EXPECT_NE(reason.find("differs"), std::string::npos) << key << ": " << reason;
    }
}

TEST(CorpusStore, RefusesACorpusBuiltWithAnotherEmbedder) {
    CorpusDir dir("embedder");
    Build(dir);
    std::string reason;
    auto staged = Embedder();
    staged.rev = "def456";
    EXPECT_EQ(CorpusStore::Open(dir.path, staged, reason), nullptr);
    EXPECT_NE(reason.find("staged embedder"), std::string::npos) << reason;
    staged = Embedder();
    staged.max_tokens = 256;
    EXPECT_EQ(CorpusStore::Open(dir.path, staged, reason), nullptr);
    EXPECT_NE(reason.find("max tokens"), std::string::npos) << reason;
}

TEST(CorpusStore, RefusesDamagedShards) {
    {
        CorpusDir dir("shard-length");
        Build(dir);
        Sql(dir,
            "PRAGMA ignore_check_constraints=ON; UPDATE guidance_vectors SET data = substr(data, "
            "5) WHERE shard = 0");
        Rehash(dir);
        EXPECT_NE(Refusal(dir).find("length"), std::string::npos);
    }
    {
        CorpusDir dir("shard-gap");
        Build(dir);
        Sql(dir, "UPDATE guidance_vectors SET first_ord = first_ord + 1 WHERE shard = 0");
        Rehash(dir);
        EXPECT_NE(Refusal(dir).find("contiguous"), std::string::npos);
    }
    {
        CorpusDir dir("shard-norm");
        Build(dir);
        // the first float becomes 2.0, so the first vector is no longer unit length
        Sql(dir,
            "UPDATE guidance_vectors SET data = unhex('00000040' || substr(hex(data), 9)) WHERE "
            "shard = 0");
        Rehash(dir);
        EXPECT_NE(Refusal(dir).find("unit length"), std::string::npos);
    }
}

TEST(CorpusStore, RefusesAForeignOrNewerFileAndAWalModeFile) {
    {
        CorpusDir dir("version");
        Build(dir);
        Sql(dir, "PRAGMA user_version=2");
        Rehash(dir);
        EXPECT_NE(Refusal(dir).find("format"), std::string::npos);
    }
    {
        CorpusDir dir("app-id");
        Build(dir);
        Sql(dir, "PRAGMA application_id=0");
        Rehash(dir);
        EXPECT_NE(Refusal(dir).find("not a corpus"), std::string::npos);
    }
    {
        CorpusDir dir("wal");
        Build(dir);
        Sql(dir, "PRAGMA journal_mode=WAL");
        Rehash(dir);
        EXPECT_NE(Refusal(dir).find("WAL"), std::string::npos);
    }
}

TEST(CorpusStore, ReportsAMissingOrBrokenManifestAsUnavailable) {
    CorpusDir dir("manifest");
    std::filesystem::create_directories(dir.path);
    EXPECT_EQ(Refusal(dir), "no manifest.json");
    std::ofstream(dir.path / kManifestFile) << "{ not json";
    EXPECT_EQ(Refusal(dir), "manifest.json does not parse");
}

TEST(CorpusStore, LoadsFromAReadOnlyDirectory) {
    CorpusDir dir("readonly");
    Build(dir);
    for (const char* name : {kCorpusFile, kManifestFile}) {
        SetFileAttributesW((dir.path / name).c_str(), FILE_ATTRIBUTE_READONLY);
    }
    std::string reason;
    const auto store = CorpusStore::Open(dir.path, Embedder(), reason);
    ASSERT_NE(store, nullptr) << reason;
    EXPECT_EQ(store->TextAt(0).text, FixtureChunks()[0].text);
}

TEST(CorpusBuilder, RefusesBadInputAndLeavesAnExistingCorpusIntact) {
    CorpusDir dir("atomic");
    Build(dir);
    const auto before = models::Sha256File(dir.path / kCorpusFile);
    auto chunks = FixtureChunks();
    auto vectors = FixtureVectors(chunks.size());
    vectors[0] *= 2.0f;
    EXPECT_THROW(BuildCorpus(dir.path, Spec(), chunks, vectors), std::invalid_argument);
    vectors = FixtureVectors(chunks.size());
    vectors.pop_back();
    EXPECT_THROW(BuildCorpus(dir.path, Spec(), chunks, vectors), std::invalid_argument);
    chunks[0].text.clear();
    EXPECT_THROW(BuildCorpus(dir.path, Spec(), chunks, FixtureVectors(chunks.size())),
                 std::invalid_argument);
    EXPECT_EQ(models::Sha256File(dir.path / kCorpusFile), before);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "corpus.db.tmp"));
    std::string reason;
    EXPECT_NE(CorpusStore::Open(dir.path, Embedder(), reason), nullptr) << reason;
}

}  // namespace
}  // namespace ambient::guidance
