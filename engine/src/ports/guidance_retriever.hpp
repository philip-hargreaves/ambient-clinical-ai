#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace ambient::guidance {

// A read-only guidance corpus the retriever can search
struct Corpus {
    std::string id;
    std::string name;
    std::string licence;
    int chunks = 0;
    std::string indexed_at;  // ISO 8601; empty until indexed
};

// One recommendation the panel shows. trigger is the note sentence that found
// it, so the clinician sees why it appeared
struct Result {
    std::string corpus;
    std::string chunk_id;   // guideline code and recommendation number, "ng100-1_1_1"
    std::string guideline;  // the code alone, "ng100"
    std::string title;
    std::string section;
    std::string text;
    double score = 0;
    std::string trigger;
};

struct Results {
    std::vector<Result> shown;
    int considered = 0;      // candidates before the floor and the filters
    bool abstained = false;  // nothing cleared the floor
};

// Retrieval for the Guidelines feature: the note in, the recommendations to
// show out, already filtered, ordered and thresholded. Retrieved text never
// enters a generated document. Uploads arrive later; until then the two
// document calls throw
class IGuidanceRetriever {
   public:
    virtual ~IGuidanceRetriever() = default;

    // Starts any slow loading in the background so the first Search is warm;
    // safe to call repeatedly
    virtual void Prepare() {}

    virtual Results Search(const std::string& note, int limit) = 0;

    virtual std::vector<Corpus> Corpora() = 0;

    virtual void AddDocument(const std::string& /*path*/) {
        throw std::logic_error("guidance documents are not supported yet");
    }

    virtual void RemoveDocument(const std::string& /*corpus_id*/) {
        throw std::logic_error("guidance documents are not supported yet");
    }
};

}  // namespace ambient::guidance
