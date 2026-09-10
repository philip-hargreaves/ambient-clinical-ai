#include "adapters/guidance/corpus_builder.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/guidance/schema.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/storage/db.hpp"
#include "core/guidance_query.hpp"

namespace ambient::guidance {
namespace {

void Require(bool ok, const std::string& why) {
    if (!ok) throw std::invalid_argument("corpus build: " + why);
}

}  // namespace

void BuildCorpus(const std::filesystem::path& dir, const CorpusSpec& spec,
                 const std::vector<Chunk>& chunks, std::span<const float> vectors) {
    Require(!spec.id.empty() && !spec.embedder_id.empty() && spec.dim > 0, "spec incomplete");
    Require(spec.source == "nice" || spec.source == "text", "source must be nice or text");
    Require(vectors.size() == chunks.size() * static_cast<std::size_t>(spec.dim),
            "vectors do not match chunks by dim");
    const auto dim = static_cast<std::size_t>(spec.dim);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        Require(!chunks[i].text.empty(), "chunk " + chunks[i].id + " has no text");
        double norm = 0;
        for (std::size_t d = 0; d < dim; ++d) {
            norm += static_cast<double>(vectors[i * dim + d]) * vectors[i * dim + d];
        }
        Require(std::fabs(norm - 1.0) <= 1e-3,
                "vector " + std::to_string(i) + " is not unit length");
    }

    std::filesystem::create_directories(dir);
    const auto final_path = dir / kCorpusFile;
    const auto temp_path = dir / (std::string(kCorpusFile) + ".tmp");
    std::filesystem::remove(temp_path);
    {
        store::Db db(temp_path, store::Db::Mode::kBuild);
        db.Exec("PRAGMA page_size=65536");
        db.Exec("PRAGMA journal_mode=DELETE");
        db.Exec("PRAGMA synchronous=OFF");
        db.Exec(("PRAGMA application_id=" + std::to_string(kCorpusApplicationId)).c_str());
        db.Exec(kCorpusSchemaSql);
        const int shard_count =
            static_cast<int>((chunks.size() + kShardVectors - 1) / kShardVectors);
        {
            store::Db::Transaction txn(db);
            auto meta = db.Prepare(
                "INSERT INTO corpus_meta(id, corpus_id, name, licence, attribution, source,"
                " embedder_id, embedder_rev, chunk_prefix, query_prefix, max_tokens, dim,"
                " vector_format, normalised, chunk_count, shard_count, built_at, builder)"
                " VALUES(1, ?, ?, ?, ?, ?, ?, ?, '', ?, ?, ?, 'f32le', 1, ?, ?, ?, ?)");
            meta.BindText(1, spec.id);
            meta.BindText(2, spec.name);
            meta.BindText(3, spec.licence);
            meta.BindText(4, spec.attribution);
            meta.BindText(5, spec.source);
            meta.BindText(6, spec.embedder_id);
            meta.BindText(7, spec.embedder_rev);
            meta.BindText(8, spec.query_prefix);
            meta.BindInt64(9, spec.max_tokens);
            meta.BindInt64(10, spec.dim);
            meta.BindInt64(11, static_cast<std::int64_t>(chunks.size()));
            meta.BindInt64(12, shard_count);
            meta.BindText(13, spec.built_at);
            meta.BindText(14, spec.builder);
            meta.Step();

            auto insert = db.Prepare(
                "INSERT INTO chunks(ord, chunk_id, code, title, chapter, number, section,"
                " update_tag, last_updated, url, text, words)"
                " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const Chunk& c = chunks[i];
                insert.Reset();
                insert.BindInt64(1, static_cast<std::int64_t>(i));
                insert.BindText(2, c.id);
                insert.BindText(3, c.code);
                insert.BindText(4, c.title);
                insert.BindText(5, c.chapter);
                insert.BindText(6, c.number);
                insert.BindText(7, c.section);
                insert.BindText(8, c.update_tag);
                insert.BindText(9, c.last_updated);
                insert.BindText(10, c.url);
                insert.BindText(11, c.text);
                insert.BindInt64(12, detail::WordCount(c.text));
                insert.Step();
            }

            auto shard = db.Prepare(
                "INSERT INTO guidance_vectors(shard, first_ord, count, dim, data) VALUES(?, ?, ?, "
                "?, ?)");
            for (int s = 0; s < shard_count; ++s) {
                const std::size_t first = static_cast<std::size_t>(s) * kShardVectors;
                const std::size_t count =
                    std::min<std::size_t>(kShardVectors, chunks.size() - first);
                const auto* bytes =
                    reinterpret_cast<const std::uint8_t*>(vectors.data() + first * dim);
                shard.Reset();
                shard.BindInt64(1, s);
                shard.BindInt64(2, static_cast<std::int64_t>(first));
                shard.BindInt64(3, static_cast<std::int64_t>(count));
                shard.BindInt64(4, spec.dim);
                shard.BindBlob(5,
                               std::span<const std::uint8_t>(bytes, count * dim * sizeof(float)));
                shard.Step();
            }
            txn.Commit();
        }
        db.Exec(("PRAGMA user_version=" + std::to_string(kCorpusFormat)).c_str());
        db.Exec("PRAGMA synchronous=FULL");
        db.Exec("VACUUM");
        db.Exec("PRAGMA optimize");
    }
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        std::filesystem::remove(temp_path);
        throw std::runtime_error("corpus build: rename failed, error " + std::to_string(error));
    }

    nlohmann::json manifest{{"format", kCorpusFormat},
                            {"id", spec.id},
                            {"name", spec.name},
                            {"licence", spec.licence},
                            {"attribution", spec.attribution},
                            {"source", spec.source},
                            {"embedder_id", spec.embedder_id},
                            {"embedder_rev", spec.embedder_rev},
                            {"query_prefix", spec.query_prefix},
                            {"max_tokens", spec.max_tokens},
                            {"dim", spec.dim},
                            {"chunks", chunks.size()},
                            {"shards", (chunks.size() + kShardVectors - 1) / kShardVectors},
                            {"file", kCorpusFile},
                            {"bytes", std::filesystem::file_size(final_path)},
                            {"sha256", models::Sha256File(final_path)},
                            {"built_at", spec.built_at},
                            {"builder", spec.builder}};
    std::ofstream out(dir / kManifestFile, std::ios::binary | std::ios::trunc);
    out << manifest.dump(2) << '\n';
    if (!out) throw std::runtime_error("corpus build: manifest write failed");
}

}  // namespace ambient::guidance
