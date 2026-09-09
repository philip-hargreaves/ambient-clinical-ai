#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/guidance_query.hpp"

namespace ambient::guidance {

// Ordering and abstention over first-stage candidates. Each sub-query's
// ranked hits vote by rank (reciprocal rank fusion); the whole-note query may
// carry more votes than one sentence; a floor on the best cosine refuses
// out-of-scope input; a population guard drops recommendations the note rules
// out. No second stage: none beat this order on the gold sets
inline constexpr double kRrfK = 60.0;
inline constexpr double kDefaultFloor = 0.85;
inline constexpr int kUnionSize = 50;

struct Hit {
    std::string id;
    double cosine = 0;
};

// One sub-query's hits in rank order
struct SubQueryHits {
    std::string query;
    bool whole_note = false;
    std::vector<Hit> hits;
};

struct Candidate {
    std::string id;
    double score = 0;     // fused vote
    double cosine = 0;    // best cosine over the sub-queries
    std::string trigger;  // the sub-query that ranked it highest
};

struct Ordered {
    std::vector<Candidate> kept;
    int considered = 0;
    bool abstained = false;
};

// The seam a second stage would fill; the shipped configuration has none
class IReranker {
   public:
    virtual ~IReranker() = default;
    virtual std::vector<double> Score(const std::string& query,
                                      const std::vector<std::string>& texts) = 0;
};

inline std::vector<Candidate> RankVote(const std::vector<SubQueryHits>& lists, int note_weight = 1,
                                       int limit = kUnionSize) {
    struct Tally {
        double score = 0;
        double cosine = -1;
        std::size_t best_rank = ~std::size_t{0};
        std::string trigger;
    };
    std::map<std::string, Tally> tally;
    for (const auto& list : lists) {
        const int votes = list.whole_note ? std::max(1, note_weight) : 1;
        for (std::size_t rank = 0; rank < list.hits.size(); ++rank) {
            auto& t = tally[list.hits[rank].id];
            t.score += votes / (kRrfK + static_cast<double>(rank) + 1.0);
            t.cosine = std::max(t.cosine, list.hits[rank].cosine);
            if (rank < t.best_rank) {
                t.best_rank = rank;
                t.trigger = list.query;
            }
        }
    }
    std::vector<Candidate> out;
    for (auto& [id, t] : tally) out.push_back({id, t.score, t.cosine, t.trigger});
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.cosine > b.cosine;
    });
    if (static_cast<int>(out.size()) > limit) out.resize(static_cast<std::size_t>(limit));
    return out;
}

// Candidates whose best cosine is under the floor are dropped; when none
// remains the search abstains and the panel shows nothing
inline Ordered ApplyFloor(std::vector<Candidate> ranked, double floor = kDefaultFloor) {
    Ordered out;
    out.considered = static_cast<int>(ranked.size());
    for (auto& c : ranked) {
        if (c.cosine >= floor) out.kept.push_back(std::move(c));
    }
    out.abstained = out.kept.empty();
    return out;
}

namespace detail {

inline std::string Erase(std::string s, std::string_view phrase) {
    for (auto pos = s.find(phrase); pos != std::string::npos; pos = s.find(phrase)) {
        s.erase(pos, phrase.size());
    }
    return s;
}

inline bool ContainsAny(std::string_view s, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (Contains(s, n)) return true;
    }
    return false;
}

// Whole-word match, so "female" is not "male" and "woman" is not "man"
inline bool ContainsWord(std::string_view s, std::string_view word) {
    for (auto pos = s.find(word); pos != std::string_view::npos; pos = s.find(word, pos + 1)) {
        const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(s[pos - 1]));
        const auto end = pos + word.size();
        const bool right = end >= s.size() || !std::isalnum(static_cast<unsigned char>(s[end]));
        if (left && right) return true;
    }
    return false;
}

inline bool ContainsAnyWord(std::string_view s, std::initializer_list<const char*> words) {
    for (const char* w : words) {
        if (ContainsWord(s, w)) return true;
    }
    return false;
}

// The first age the note states ("42-year-old", "65 years old", "aged 72"), or -1
inline int StatedAge(std::string_view lower) {
    if (const auto aged = lower.find("aged "); aged != std::string_view::npos) {
        int age = 0, digits = 0;
        for (auto k = aged + 5;
             k < lower.size() && std::isdigit(static_cast<unsigned char>(lower[k])) && digits < 3;
             ++k, ++digits) {
            age = age * 10 + (lower[k] - '0');
        }
        if (digits > 0) return age;
    }
    for (std::size_t i = 0; i < lower.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(lower[i]))) continue;
        std::size_t j = i;
        int age = 0;
        while (j < lower.size() && std::isdigit(static_cast<unsigned char>(lower[j])) &&
               j - i < 3) {
            age = age * 10 + (lower[j] - '0');
            ++j;
        }
        const auto rest = lower.substr(j);
        if (StartsWith(rest, "-year-old") || StartsWith(rest, " year old") ||
            StartsWith(rest, " years old") || StartsWith(rest, "-year old") ||
            StartsWith(rest, "yo ") || StartsWith(rest, " yo")) {
            return age;
        }
        i = j;
    }
    return -1;
}

}  // namespace detail

// A recommendation addressed to a population the note explicitly rules out:
// pregnancy against "not pregnant", children against a stated adult age, one
// sex against the other. Only explicit statements count; silence never does
inline bool PopulationConflict(std::string_view note, std::string_view recommendation) {
    const auto n = detail::Lower(note);
    auto r = detail::Lower(recommendation);
    r = detail::Erase(detail::Erase(r, "not pregnant"), "non-pregnant");
    if (detail::ContainsAny(r, {"pregnant", "pregnancy"}) &&
        detail::ContainsAny(n, {"not pregnant", "non-pregnant", "no pregnancy"})) {
        return true;
    }
    const int age = detail::StatedAge(n);
    const bool adult = age >= 18 || detail::ContainsAnyWord(n, {"adult", "adults", "man", "woman"});
    if (adult &&
        (detail::ContainsAnyWord(r, {"children", "child", "infant", "infants", "paediatric"}) ||
         detail::ContainsAny(r, {"young people", "under 16", "under 18"}))) {
        return true;
    }
    const bool female = detail::ContainsAnyWord(n, {"woman", "women", "female", "she", "her"});
    const bool male = detail::ContainsAnyWord(n, {"man", "men", "male", "he", "his"});
    if (female && !male && detail::ContainsAnyWord(r, {"man", "men", "male", "males"})) return true;
    if (male && !female && detail::ContainsAnyWord(r, {"woman", "women", "female", "females"}))
        return true;
    return false;
}

// "NG100, 1.1 Referral: Rheumatoid arthritis in adults: management"
inline std::string Citation(std::string_view code, std::string_view section,
                            std::string_view title) {
    std::string out(code);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!section.empty()) out += ", " + std::string(section);
    if (!title.empty()) out += ": " + std::string(title);
    return out;
}

}  // namespace ambient::guidance
