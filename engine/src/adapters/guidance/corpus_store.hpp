#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adapters/storage/db.hpp"

namespace ambient::guidance {

// The staged embedder a corpus must have been built with
struct EmbedderIdentity {
    std::string id;
    std::string rev;  // sha256 of the weights
    int dim = 0;
    int max_tokens = 0;
    std::string query_prefix;
};

struct CorpusInfo {
    std::string id;
    std::string name;
    std::string licence;
    std::string attribution;
    std::string source;
    std::string embedder_id;
    std::string embedder_rev;
    std::string built_at;
    std::string sha256;  // of corpus.db, from the manifest, verified at open
    int dim = 0;
    std::int64_t chunk_count = 0;
    std::filesystem::path dir;
};

// What every result needs without a read
struct Cite {
    std::string chunk_id;
    std::string code;
    std::string title;
    std::string chapter;
    std::string number;
    std::string section;
};

struct ChunkText {
    std::string text;
    std::string url;
    std::string last_updated;
    std::string update_tag;
};

// A corpus directory opened read-only, its vectors resident as one matrix,
// its text read on demand. Every claim the manifest makes is asserted against
// the file and the staged embedder before a vector is trusted; a corpus that
// fails a guard is unavailable with a reason, never a throw into the lane.
// One thread owns an instance
class CorpusStore {
   public:
    static std::unique_ptr<CorpusStore> Open(const std::filesystem::path& dir,
                                             const EmbedderIdentity& embedder, std::string& reason);

    const CorpusInfo& Info() const {
        return info_;
    }
    std::size_t Size() const {
        return cites_.size();
    }
    int Dim() const {
        return info_.dim;
    }
    // Row-major Size() by Dim(), unit vectors
    const float* Matrix() const {
        return matrix_.data();
    }
    const Cite& CiteAt(std::size_t ord) const {
        return cites_[ord];
    }
    ChunkText TextAt(std::size_t ord);

   private:
    CorpusStore(store::Db db, CorpusInfo info);

    store::Db db_;
    CorpusInfo info_;
    std::vector<float> matrix_;
    std::vector<Cite> cites_;
    store::Db::Stmt text_;
};

}  // namespace ambient::guidance
