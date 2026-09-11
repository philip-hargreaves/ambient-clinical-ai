#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/embedder.hpp"
#include "core/guidance_rank.hpp"
#include "ports/guidance_retriever.hpp"

namespace ambient::guidance {

struct RetrieverOptions {
    double floor = kDefaultFloor;
    int note_weight = 1;
    int union_size = kUnionSize;
};

using EmbedderLoader = std::function<std::unique_ptr<IEmbedder>()>;

// The shipped retriever: one embedder, every corpus under corpora_root that
// passes the load guards, exact scan, rank vote across the sub-queries, cosine
// floor, population guard. A result's score is its best cosine; the order is
// the vote. Prepare and Search run one at a time; Corpora may be read from any
// thread. A load failure is kept and rethrown, never retried: the model store
// does not change while the engine runs
class Retriever : public IGuidanceRetriever {
   public:
    Retriever(EmbedderLoader load_embedder, std::filesystem::path corpora_root,
              RetrieverOptions options = {});

    void Prepare() override;
    Results Search(const std::string& note, int limit) override;
    std::vector<Corpus> Corpora() override;

   private:
    struct Loaded {
        Corpus corpus;
        std::unique_ptr<CorpusStore> store;  // null when unavailable
    };
    void Load();

    EmbedderLoader load_embedder_;
    std::filesystem::path corpora_root_;
    RetrieverOptions options_;

    std::mutex search_mutex_;
    std::unique_ptr<IEmbedder> embedder_;
    std::vector<Loaded> loaded_;
    std::string load_error_;

    std::mutex corpora_mutex_;
    std::vector<Corpus> corpora_;
};

}  // namespace ambient::guidance
