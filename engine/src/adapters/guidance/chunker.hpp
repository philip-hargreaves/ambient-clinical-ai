#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace ambient::guidance {

// One retrievable unit: a recommendation from a structured guideline, or a
// paragraph run from plain text. The same record the evaluation harness indexed
struct Chunk {
    std::string id;  // "<code>-<number, dots as underscores>" or "<code>-<n>"
    std::string code;
    std::string title;
    std::string chapter;
    std::string number;
    std::string section;
    std::string update_tag;
    std::string last_updated;
    std::string text;
    std::string url;
    std::string source;  // "nice" or "text"
};

inline constexpr int kTargetWords = 300;
inline constexpr int kMaxWords = 600;
inline constexpr int kMinParagraphWords = 5;

// A manifest entry is indexed when its code was requested, it is current and
// not a stub page
bool IncludeDocument(const nlohmann::json& manifest_entry, const std::string& code,
                     const std::set<std::string>& requested_codes);

// One chunk per recommendation of a structured guideline document. Ids already
// in seen are skipped: some guidelines render a chapter twice
std::vector<Chunk> ChunksFromDocument(const nlohmann::json& doc, std::set<std::string>& seen);

// "Recommendation 12", "recommendation 3a", "1.2", "1.2.3 Offer": a paragraph
// that opens a numbered recommendation
bool StartsRecommendation(const std::string& paragraph);

// Plain or markdown text: paragraphs split on blank lines and collapsed to one
// line; runs close at a recommendation heading, at kTargetWords, or before
// kMaxWords; paragraphs under kMinParagraphWords are dropped
std::vector<Chunk> ChunksFromText(const std::string& code, const std::string& title,
                                  const std::string& text, const std::string& url = "");

}  // namespace ambient::guidance
