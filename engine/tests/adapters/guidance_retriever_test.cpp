#include <gtest/gtest.h>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapters/guidance/retriever.hpp"
#include "guidance_fixture.hpp"

namespace ambient::guidance {
namespace {

constexpr const char* kFixtureDir = AMBIENT_GUIDANCE_FIXTURE_DIR;
constexpr int kDim = 256;

// Hashed bag of words, four letters and up, unit length: texts sharing words
// score high, so the fixture notes find their guidelines without a model
struct WordEmbedder : IEmbedder {
    EmbedderIdentity identity{"fx-words", "rev-a", kDim, 512, ""};
    const EmbedderIdentity& Identity() const override {
        return identity;
    }
    Embedding Embed(const std::string& text) override {
        Embedding out;
        out.vector.assign(kDim, 0.f);
        std::string word;
        auto flush = [&] {
            if (word.size() >= 4) {
                std::uint32_t h = 2166136261u;
                for (unsigned char c : word) h = (h ^ c) * 16777619u;
                out.vector[h % kDim] += 1.f;
                ++out.tokens;
            }
            word.clear();
        };
        for (unsigned char c : text) {
            if (std::isalpha(c)) {
                word.push_back(static_cast<char>(std::tolower(c)));
            } else {
                flush();
            }
        }
        flush();
        double norm = 0;
        for (float v : out.vector) norm += static_cast<double>(v) * v;
        if (norm == 0) {
            out.vector[0] = 1.f;
        } else {
            for (auto& v : out.vector) v /= static_cast<float>(std::sqrt(norm));
        }
        return out;
    }
};

// Two live corpora splitting the fixture guidelines, one built under another
// weights revision, one folder that is not a corpus
struct Root {
    fixture::TempDir dir{"root"};
    Root() {
        WordEmbedder live;
        fixture::Build(dir.path / "fixture-a", "fixture-a", live,
                       fixture::Chunks(kFixtureDir, {"fx100", "fx200"}));
        fixture::Build(dir.path / "fixture-b", "fixture-b", live,
                       fixture::Chunks(kFixtureDir, {"fx300", "fx400"}));
        WordEmbedder stale;
        stale.identity.rev = "rev-b";
        fixture::Build(dir.path / "stale", "stale", stale, fixture::Chunks(kFixtureDir, {"fx100"}));
        std::filesystem::create_directories(dir.path / "notes");
    }
    std::unique_ptr<Retriever> Make(double floor = 0.2) {
        RetrieverOptions options;
        options.floor = floor;
        return std::make_unique<Retriever>([] { return std::make_unique<WordEmbedder>(); },
                                           dir.path, options);
    }
};

std::string NoteText(const char* id) {
    for (const auto& note : fixture::Notes(kFixtureDir)) {
        if (note.id == id) return note.text;
    }
    throw std::runtime_error(std::string("no fixture note ") + id);
}

TEST(Retriever, ListsEveryCorpusDirectoryWithTheStaleOneUnavailable) {
    Root root;
    auto retriever = root.Make();
    retriever->Prepare();
    const auto corpora = retriever->Corpora();
    ASSERT_EQ(corpora.size(), 3u);
    EXPECT_EQ(corpora[0].id, "fixture-a");
    EXPECT_EQ(corpora[0].chunks,
              static_cast<int>(fixture::Chunks(kFixtureDir, {"fx100", "fx200"}).size()));
    EXPECT_EQ(corpora[0].embedder, "fx-words");
    EXPECT_EQ(corpora[0].built_at, "2026-09-11T00:00:00Z");
    EXPECT_EQ(corpora[0].sha256.size(), 64u);
    EXPECT_TRUE(corpora[0].unavailable.empty());
    EXPECT_EQ(corpora[1].id, "fixture-b");
    EXPECT_TRUE(corpora[1].unavailable.empty());
    EXPECT_EQ(corpora[2].id, "stale");
    EXPECT_FALSE(corpora[2].unavailable.empty());
    EXPECT_EQ(corpora[2].chunks, 0);
}

TEST(Retriever, CitesTheGuidelineTheNoteDescribes) {
    Root root;
    auto retriever = root.Make();
    const auto note = NoteText("joint-referral");
    const auto results = retriever->Search(note, 3);
    ASSERT_FALSE(results.shown.empty());
    EXPECT_LE(results.shown.size(), 3u);
    EXPECT_GE(results.considered, static_cast<int>(results.shown.size()));
    EXPECT_FALSE(results.abstained);
    EXPECT_EQ(results.shown[0].guideline, "fx100");
    for (const auto& r : results.shown) {
        EXPECT_EQ(r.corpus, "fixture-a");
        EXPECT_FALSE(r.chunk_id.empty());
        EXPECT_FALSE(r.title.empty());
        EXPECT_FALSE(r.section.empty());
        EXPECT_FALSE(r.text.empty());
        EXPECT_GE(r.score, 0.2);
        EXPECT_NE(note.find(r.trigger), std::string::npos) << "trigger is a sentence of the note";
    }
}

TEST(Retriever, MergesHitsFromEveryCorpusIntoOneList) {
    Root root;
    auto retriever = root.Make();
    const auto note =
        "Synovitis of the small joints of both hands with morning stiffness. "
        "Also reports frequent migraine with aura and asks about a triptan.";
    const auto results = retriever->Search(note, 6);
    bool from_a = false, from_b = false;
    for (const auto& r : results.shown) {
        from_a |= r.corpus == "fixture-a";
        from_b |= r.corpus == "fixture-b";
    }
    EXPECT_TRUE(from_a);
    EXPECT_TRUE(from_b);
}

TEST(Retriever, AbstainsWhenNothingClearsTheFloor) {
    Root root;
    auto retriever = root.Make(0.999);
    const auto results = retriever->Search(NoteText("joint-referral"), 3);
    EXPECT_TRUE(results.abstained);
    EXPECT_TRUE(results.shown.empty());
    EXPECT_GT(results.considered, 0);
}

TEST(Retriever, LimitBoundsWhatIsShown) {
    Root root;
    auto retriever = root.Make();
    EXPECT_EQ(retriever->Search(NoteText("joint-referral"), 1).shown.size(), 1u);
    EXPECT_TRUE(retriever->Search(NoteText("joint-referral"), 0).shown.empty());
}

TEST(Retriever, AnEmptyNoteAbstainsWithoutEmbedding) {
    Root root;
    auto retriever = root.Make();
    const auto results = retriever->Search("  ", 3);
    EXPECT_TRUE(results.abstained);
    EXPECT_EQ(results.considered, 0);
}

TEST(Retriever, KeepsALoadFailureAndRethrowsIt) {
    int loads = 0;
    Retriever retriever(
        [&]() -> std::unique_ptr<IEmbedder> {
            ++loads;
            throw std::runtime_error("no embedding model staged");
        },
        std::filesystem::temp_directory_path());
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            retriever.Search("Chest pain on exertion.", 3);
            FAIL() << "search succeeded without an embedder";
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ(e.what(), "no embedding model staged");
        }
    }
    EXPECT_EQ(loads, 1);
    EXPECT_TRUE(retriever.Corpora().empty());
}

TEST(Retriever, AMissingRootHasNoCorporaAndReturnsNothing) {
    Retriever retriever([] { return std::make_unique<WordEmbedder>(); },
                        std::filesystem::temp_directory_path() / "ambient-retriever-none");
    retriever.Prepare();
    EXPECT_TRUE(retriever.Corpora().empty());
    const auto results = retriever.Search("Chest pain on exertion.", 3);
    EXPECT_TRUE(results.shown.empty());
    EXPECT_FALSE(results.abstained);
    EXPECT_EQ(results.considered, 0);
}

TEST(Retriever, DocumentCallsAreNotSupportedYet) {
    Root root;
    auto retriever = root.Make();
    EXPECT_THROW(retriever->AddDocument("letter.pdf"), std::logic_error);
    EXPECT_THROW(retriever->RemoveDocument("upload-1"), std::logic_error);
}

}  // namespace
}  // namespace ambient::guidance
