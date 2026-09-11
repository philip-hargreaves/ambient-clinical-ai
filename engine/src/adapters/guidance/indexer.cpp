#include "adapters/guidance/indexer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace ambient::guidance {
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("cannot read " + path.string());
    return nlohmann::json::parse(in);
}

std::vector<std::filesystem::path> SortedFiles(const std::filesystem::path& dir,
                                               const std::set<std::string>& extensions) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && extensions.count(Lower(entry.path().extension().string()))) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The first markdown heading, or empty
std::string FirstHeading(const std::string& text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        const auto line =
            text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line[0] == '#') {
            const auto start = line.find_first_not_of("# \t");
            if (start != std::string::npos) return line.substr(start);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return "";
}

}  // namespace

BuildSpec ReadBuildSpec(const std::filesystem::path& json) {
    const auto spec = ReadJson(json);
    BuildSpec out;
    out.corpus.id = spec.at("id").get<std::string>();
    out.corpus.name = spec.at("name").get<std::string>();
    out.corpus.licence = spec.at("licence").get<std::string>();
    out.corpus.attribution = spec.at("attribution").get<std::string>();
    out.corpus.source = spec.at("source").get<std::string>();
    const auto base = json.parent_path();
    if (out.corpus.source == "nice") {
        const auto& nice = spec.at("nice");
        out.nice_dir = base / nice.at("dir").get<std::string>();
        out.codes = base / nice.at("codes").get<std::string>();
    } else if (out.corpus.source == "text") {
        out.text_dir = base / spec.at("text").at("dir").get<std::string>();
    } else {
        throw std::runtime_error("build spec: source must be nice or text");
    }
    return out;
}

std::set<std::string> ReadCodes(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in.is_open()) throw std::runtime_error("cannot read " + file.string());
    std::set<std::string> codes;
    for (std::string line; std::getline(in, line);) {
        auto code = line.substr(0, line.find('#'));
        const auto end = code.find_last_not_of(" \t\r");
        if (end == std::string::npos) continue;
        code.erase(end + 1);
        code.erase(0, code.find_first_not_of(" \t"));
        if (!code.empty()) codes.insert(Lower(code));
    }
    return codes;
}

std::vector<Chunk> ChunksFromNiceDir(const std::filesystem::path& dir,
                                     const std::set<std::string>& requested) {
    const auto manifest = ReadJson(dir / "manifest.json");
    std::vector<Chunk> out;
    std::set<std::string> seen;
    for (const auto& path : SortedFiles(dir / "json", {".json"})) {
        const auto doc = ReadJson(path);
        const auto code = doc.at("code").get<std::string>();
        const auto entry = manifest.find(code);
        if (entry == manifest.end() || !IncludeDocument(*entry, code, requested)) continue;
        auto chunks = ChunksFromDocument(doc, seen);
        out.insert(out.end(), std::move_iterator(chunks.begin()), std::move_iterator(chunks.end()));
    }
    return out;
}

std::vector<Chunk> ChunksFromTextDir(const std::filesystem::path& dir) {
    std::vector<Chunk> out;
    for (const auto& path : SortedFiles(dir, {".md", ".txt"})) {
        const auto text = ReadFile(path);
        const auto code = Lower(path.stem().string());
        auto title = FirstHeading(text);
        if (title.empty()) title = path.stem().string();
        auto chunks = ChunksFromText(code, title, text, path.filename().string());
        out.insert(out.end(), std::move_iterator(chunks.begin()), std::move_iterator(chunks.end()));
    }
    return out;
}

IndexReport IndexCorpus(const BuildSpec& spec, IEmbedder& embedder,
                        const std::filesystem::path& out_dir, const std::string& built_at,
                        const std::string& builder, const Progress& progress) {
    const auto chunks = spec.corpus.source == "nice"
                            ? ChunksFromNiceDir(spec.nice_dir, ReadCodes(spec.codes))
                            : ChunksFromTextDir(spec.text_dir);
    if (chunks.empty()) throw std::runtime_error("build spec: the source yields no chunks");
    const auto& identity = embedder.Identity();
    const auto dim = static_cast<std::size_t>(identity.dim);
    std::vector<float> vectors;
    vectors.reserve(chunks.size() * dim);
    IndexReport report;
    report.chunks = chunks.size();
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        auto embedding = embedder.Embed(chunks[i].text);
        if (embedding.vector.size() != dim) {
            throw std::runtime_error("embedding of " + chunks[i].id + " has the wrong dimension");
        }
        if (embedding.truncated) ++report.truncated;
        vectors.insert(vectors.end(), embedding.vector.begin(), embedding.vector.end());
        if (progress) progress(i + 1, chunks.size());
    }
    CorpusSpec corpus = spec.corpus;
    corpus.embedder_id = identity.id;
    corpus.embedder_rev = identity.rev;
    corpus.query_prefix = identity.query_prefix;
    corpus.max_tokens = identity.max_tokens;
    corpus.dim = identity.dim;
    corpus.built_at = built_at;
    corpus.builder = builder;
    BuildCorpus(out_dir, corpus, chunks, vectors);
    return report;
}

}  // namespace ambient::guidance
