#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "adapters/guidance/chunker.hpp"

namespace ambient::guidance {

inline constexpr std::uint32_t kCorpusApplicationId = 0x414D4247;  // "AMBG"
inline constexpr int kCorpusFormat = 1;
inline constexpr int kShardVectors = 256;  // 1 MB a shard at 1024 dimensions
inline constexpr const char* kCorpusFile = "corpus.db";
inline constexpr const char* kManifestFile = "manifest.json";

// What the indexer knows about a corpus that the chunks do not carry
struct CorpusSpec {
    std::string id;  // carries the fetch date, "nice-2026-08-25"
    std::string name;
    std::string licence;
    std::string attribution;
    std::string source;  // "nice" or "text"
    std::string embedder_id;
    std::string embedder_rev;  // sha256 of the model weights
    std::string query_prefix;
    int max_tokens = 512;
    int dim = 0;
    std::string built_at;  // ISO 8601 UTC
    std::string builder;
};

// Writes corpus.db and manifest.json into dir. The database is built as a
// temp file with the journal off, vacuumed, switched to a rollback journal and
// renamed into place, so dir holds either the old corpus or the new one.
// vectors is row-major, chunks.size() by spec.dim, unit length. Throws on any
// failure and leaves dir as it was
void BuildCorpus(const std::filesystem::path& dir, const CorpusSpec& spec,
                 const std::vector<Chunk>& chunks, std::span<const float> vectors);

}  // namespace ambient::guidance
