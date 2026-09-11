#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/models/model_store.hpp"

namespace ambient::guidance {

struct Embedding {
    std::vector<float> vector;  // unit length, Identity().dim floats
    std::size_t tokens = 0;     // before truncation
    bool truncated = false;     // the text ran past max_tokens
};

// The seam the indexer and the retriever share; one text at a time
class IEmbedder {
   public:
    virtual ~IEmbedder() = default;
    virtual const EmbedderIdentity& Identity() const = 0;
    virtual Embedding Embed(const std::string& text) = 0;
};

inline constexpr int kEmbedMaxTokens = 512;

// The staged embedding model on the CPU through the GenAI pipeline: mean
// pooling, normalised, no instruction strings. Load verifies the files and
// runs the startup guards; it throws naming the failure
class Embedder : public IEmbedder {
   public:
    static std::unique_ptr<Embedder> Load(const models::ModelStore& store,
                                          const std::string& tier = "default");
    ~Embedder() override;

    const EmbedderIdentity& Identity() const override;
    Embedding Embed(const std::string& text) override;

   private:
    struct Impl;
    explicit Embedder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace ambient::guidance
