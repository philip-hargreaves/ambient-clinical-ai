#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ambient::guidance {

// Sub-queries for retrieval: each sentence of the note on its own, plus the
// whole note. The boundaries mirror the evaluation harness's splitter so the
// engine and the harness build the same candidate lists; common note
// abbreviations do not end a sentence
inline constexpr int kMinSentenceWords = 3;

namespace detail {

inline std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline int WordCount(std::string_view s) {
    int words = 0;
    bool in_word = false;
    for (unsigned char c : s) {
        const bool space = std::isspace(c) != 0;
        if (!space && !in_word) ++words;
        in_word = !space;
    }
    return words;
}

inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// The token before a full stop that does not close a sentence
inline bool IsAbbreviation(std::string_view token) {
    static const char* const kWords[] = {"e.g",    "i.e", "dr", "mr", "mrs", "ms",  "prof", "vs",
                                         "approx", "etc", "no", "hx", "pt",  "rx",  "mg",   "mcg",
                                         "ml",     "kg",  "cm", "mm", "bd",  "tds", "od",   "prn",
                                         "st",     "ca",  "cf", "wk", "wks", "yr",  "yrs",  "mth"};
    const auto lower = Lower(token);
    if (lower.size() == 1 && std::isalpha(static_cast<unsigned char>(lower[0]))) return true;
    for (const char* k : kWords) {
        if (lower == k) return true;
    }
    return false;
}

inline std::string_view TokenBefore(std::string_view text, std::size_t pos) {
    std::size_t start = pos;
    while (start > 0 && !std::isspace(static_cast<unsigned char>(text[start - 1]))) --start;
    return text.substr(start, pos - start);
}

inline bool OpensSentence(unsigned char c) {
    return std::isupper(c) || std::isdigit(c) || c == '"' || c == '\'' || c == '(';
}

inline bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

inline bool Contains(std::string_view s, std::string_view needle) {
    return s.find(needle) != std::string_view::npos;
}

}  // namespace detail

// A newline always ends a sentence; ., ! or ? ends one when whitespace and a
// capital, digit, quote or bracket follow and the word before is not an
// abbreviation. Fragments under kMinSentenceWords are dropped
inline std::vector<std::string> SplitSentences(std::string_view note) {
    std::vector<std::string> out;
    auto flush = [&](std::size_t from, std::size_t to) {
        const auto piece = detail::Trim(note.substr(from, to - from));
        if (detail::WordCount(piece) >= kMinSentenceWords) out.emplace_back(piece);
    };
    std::size_t start = 0;
    for (std::size_t i = 0; i < note.size(); ++i) {
        const char c = note[i];
        if (c == '\n') {
            flush(start, i);
            start = i + 1;
            continue;
        }
        if (c != '.' && c != '!' && c != '?') continue;
        std::size_t j = i + 1;
        while (j < note.size() && (note[j] == ' ' || note[j] == '\t' || note[j] == '\r')) ++j;
        if (j == i + 1 || j >= note.size()) continue;
        if (!detail::OpensSentence(static_cast<unsigned char>(note[j]))) continue;
        if (c == '.' && detail::IsAbbreviation(detail::TokenBefore(note, i))) continue;
        flush(start, i + 1);
        start = j;
    }
    flush(start, note.size());
    return out;
}

// A sentence that must not retrieve: it records a negated finding, family
// history or a hypothetical, so guidance for that condition would be for a
// patient who does not have it. Conservative: only openings and unambiguous
// phrases count
inline bool IsExcluded(std::string_view sentence) {
    const auto s = detail::Lower(detail::Trim(sentence));
    static const char* const kOpenings[] = {
        "no ",      "nil ",         "not ",       "denies",         "denied",    "never ",
        "without ", "negative for", "no history", "family history", "fh:",       "fh ",
        "fhx",      "if ",          "unless ",    "should she",     "should he", "in case"};
    for (const char* k : kOpenings) {
        if (detail::StartsWith(s, k)) return true;
    }
    static const char* const kPhrases[] = {" mother had",    " father had",    " mother has",
                                           " father has",    " sister had",    " brother had",
                                           "grandmother",    "grandfather",    "family history of",
                                           "no evidence of", "were to develop"};
    for (const char* k : kPhrases) {
        if (detail::Contains(s, k)) return true;
    }
    return false;
}

// The sentences that pass the filter, then the whole note as one more query
inline std::vector<std::string> SubQueries(std::string_view note) {
    std::vector<std::string> out;
    for (auto& sentence : SplitSentences(note)) {
        if (!IsExcluded(sentence)) out.push_back(std::move(sentence));
    }
    const auto whole = detail::Trim(note);
    if (detail::WordCount(whole) >= kMinSentenceWords &&
        std::find(out.begin(), out.end(), whole) == out.end()) {
        out.emplace_back(whole);
    }
    return out;
}

}  // namespace ambient::guidance
