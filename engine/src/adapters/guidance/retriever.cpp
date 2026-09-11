#include "adapters/guidance/retriever.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "adapters/guidance/corpus_builder.hpp"
#include "core/guidance_query.hpp"
#include "core/guidance_scan.hpp"

namespace ambient::guidance {
namespace {

Corpus Describe(const CorpusInfo& info) {
    Corpus c;
    c.id = info.id;
    c.name = info.name;
    c.licence = info.licence;
    c.attribution = info.attribution;
    c.source = info.source;
    c.embedder = info.embedder_id;
    c.sha256 = info.sha256;
    c.chunks = static_cast<int>(info.chunk_count);
    c.built_at = info.built_at;
    return c;
}

// A hit's place across the corpora, keyed for the vote
struct Located {
    std::size_t corpus = 0;
    std::size_t ord = 0;
    float cosine = 0;
};

std::string Key(const Located& l) {
    return std::to_string(l.corpus) + ":" + std::to_string(l.ord);
}

}  // namespace

Retriever::Retriever(EmbedderLoader load_embedder, std::filesystem::path corpora_root,
                     RetrieverOptions options)
    : load_embedder_(std::move(load_embedder)),
      corpora_root_(std::move(corpora_root)),
      options_(options) {}

void Retriever::Prepare() {
    std::lock_guard<std::mutex> lock(search_mutex_);
    Load();
}

void Retriever::Load() {
    if (embedder_) return;
    if (!load_error_.empty()) throw std::runtime_error(load_error_);
    try {
        auto embedder = load_embedder_();
        if (!embedder) throw std::runtime_error("no guidance embedder");
        std::vector<std::filesystem::path> dirs;
        if (std::filesystem::is_directory(corpora_root_)) {
            for (const auto& entry : std::filesystem::directory_iterator(corpora_root_)) {
                if (entry.is_directory() && std::filesystem::exists(entry.path() / kManifestFile)) {
                    dirs.push_back(entry.path());
                }
            }
        }
        std::sort(dirs.begin(), dirs.end());
        std::vector<Loaded> loaded;
        std::vector<Corpus> corpora;
        for (const auto& dir : dirs) {
            Loaded item;
            std::string reason;
            item.store = CorpusStore::Open(dir, embedder->Identity(), reason);
            if (item.store) {
                item.corpus = Describe(item.store->Info());
            } else {
                item.corpus.id = dir.filename().string();
                item.corpus.unavailable = reason;
            }
            corpora.push_back(item.corpus);
            loaded.push_back(std::move(item));
        }
        embedder_ = std::move(embedder);
        loaded_ = std::move(loaded);
        std::lock_guard<std::mutex> lock(corpora_mutex_);
        corpora_ = std::move(corpora);
    } catch (const std::exception& e) {
        load_error_ = e.what();
        throw;
    }
}

Results Retriever::Search(const std::string& note, int limit) {
    std::lock_guard<std::mutex> lock(search_mutex_);
    Load();
    Results out;
    const auto queries = SubQueries(note);
    if (queries.empty()) {
        out.abstained = true;
        return out;
    }
    std::vector<CorpusStore*> stores;
    for (auto& item : loaded_) {
        if (item.store) stores.push_back(item.store.get());
    }
    if (stores.empty()) return out;

    // Every corpus shares the embedder, so one sub-query's hits from all of them
    // sort into one list before the vote
    const std::string whole(detail::Trim(note));
    const int k = options_.union_size;
    std::map<std::string, Located> where;
    std::vector<SubQueryHits> lists;
    for (const auto& query : queries) {
        const auto embedding = embedder_->Embed(query);
        std::vector<Located> located;
        for (std::size_t c = 0; c < stores.size(); ++c) {
            const auto hits = Scan(stores[c]->Matrix(), stores[c]->Size(), stores[c]->Dim(),
                                   embedding.vector.data(), k);
            for (const auto& hit : hits) located.push_back({c, hit.ord, hit.cosine});
        }
        std::stable_sort(located.begin(), located.end(),
                         [](const Located& a, const Located& b) { return a.cosine > b.cosine; });
        if (located.size() > static_cast<std::size_t>(k))
            located.resize(static_cast<std::size_t>(k));
        SubQueryHits list{query, query == whole, {}};
        for (const auto& l : located) {
            auto key = Key(l);
            list.hits.push_back({key, l.cosine});
            where.emplace(std::move(key), l);
        }
        lists.push_back(std::move(list));
    }

    auto ordered = ApplyFloor(RankVote(lists, options_.note_weight, k), options_.floor);
    out.considered = ordered.considered;
    out.abstained = ordered.abstained;
    for (const auto& candidate : ordered.kept) {
        if (static_cast<int>(out.shown.size()) >= limit) break;
        const auto& at = where.at(candidate.id);
        auto* store = stores[at.corpus];
        auto chunk = store->TextAt(at.ord);
        if (PopulationConflict(note, chunk.text)) continue;
        const auto& cite = store->CiteAt(at.ord);
        Result result;
        result.corpus = store->Info().id;
        result.chunk_id = cite.chunk_id;
        result.guideline = cite.code;
        result.title = cite.title;
        result.section = cite.section.empty() ? cite.number : cite.section;
        result.text = std::move(chunk.text);
        result.score = candidate.cosine;
        result.trigger = candidate.trigger;
        out.shown.push_back(std::move(result));
    }
    return out;
}

std::vector<Corpus> Retriever::Corpora() {
    std::lock_guard<std::mutex> lock(corpora_mutex_);
    return corpora_;
}

}  // namespace ambient::guidance
