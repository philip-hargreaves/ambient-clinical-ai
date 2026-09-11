#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_builder.hpp"
#include "adapters/guidance/embedder.hpp"

// The invented fixture corpus and notes, and a corpus directory built from
// them with any embedder
namespace ambient::guidance::fixture {

// A fresh temp directory, removed on scope exit
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name)
        : path(std::filesystem::temp_directory_path() / ("ambient-guidance-" + std::string(name))) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

inline std::ifstream Open(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("missing guidance fixture " + path.string());
    return in;
}

// Every fixture recommendation, or those of the given guideline codes
inline std::vector<Chunk> Chunks(const std::filesystem::path& fixture_dir,
                                 const std::set<std::string>& codes = {}) {
    auto in = Open(fixture_dir / "corpus.jsonl");
    std::vector<Chunk> chunks;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const auto row = nlohmann::json::parse(line);
        Chunk c;
        c.id = row.at("id");
        c.code = row.at("code");
        if (!codes.empty() && !codes.count(c.code)) continue;
        c.title = row.at("title");
        c.section = row.at("section");
        c.text = row.at("text");
        c.url = "https://example.test/" + c.id;
        c.source = "text";
        chunks.push_back(std::move(c));
    }
    return chunks;
}

struct Note {
    std::string id;
    std::string text;
    std::vector<std::string> expected;  // chunk ids a search should surface
    std::vector<std::string> must_not;  // guideline codes it must not
};

inline std::vector<Note> Notes(const std::filesystem::path& fixture_dir) {
    auto in = Open(fixture_dir / "notes.jsonl");
    std::vector<Note> notes;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const auto row = nlohmann::json::parse(line);
        notes.push_back({row.at("id"), row.at("text"), row.at("expected"), row.at("must_not")});
    }
    return notes;
}

// The fixture corpus as one markdown file per guideline under docs, each
// recommendation numbered so the text chunker keeps them apart
inline void WriteMarkdown(const std::filesystem::path& fixture_dir,
                          const std::filesystem::path& docs) {
    std::map<std::string, std::string> per_code;
    for (const auto& chunk : Chunks(fixture_dir)) {
        auto number = chunk.id.substr(chunk.code.size() + 1);
        std::replace(number.begin(), number.end(), '_', '.');
        per_code[chunk.code] += number + " " + chunk.text + "\n\n";
    }
    std::filesystem::create_directories(docs);
    for (const auto& [code, text] : per_code) {
        std::ofstream(docs / (code + ".md"), std::ios::binary) << text;
    }
}

// Embeds the chunks and writes corpus.db and manifest.json into dir
inline void Build(const std::filesystem::path& dir, const std::string& id, IEmbedder& embedder,
                  const std::vector<Chunk>& chunks) {
    const auto& identity = embedder.Identity();
    std::vector<float> vectors;
    for (const auto& chunk : chunks) {
        const auto embedding = embedder.Embed(chunk.text);
        vectors.insert(vectors.end(), embedding.vector.begin(), embedding.vector.end());
    }
    CorpusSpec spec;
    spec.id = id;
    spec.name = "Fixture guidance corpus";
    spec.licence = "invented";
    spec.attribution = "none";
    spec.source = "text";
    spec.embedder_id = identity.id;
    spec.embedder_rev = identity.rev;
    spec.query_prefix = identity.query_prefix;
    spec.max_tokens = identity.max_tokens;
    spec.dim = identity.dim;
    spec.built_at = "2026-09-11T00:00:00Z";
    spec.builder = "tests";
    BuildCorpus(dir, spec, chunks, vectors);
}

}  // namespace ambient::guidance::fixture
