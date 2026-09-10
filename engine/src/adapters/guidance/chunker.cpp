#include "adapters/guidance/chunker.hpp"

#include <cctype>
#include <string_view>

#include "core/guidance_query.hpp"

namespace ambient::guidance {
namespace {

// Whitespace runs to one space
std::string Collapse(std::string_view s) {
    std::string out;
    bool space = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!space) out.push_back(' ');
            space = true;
        } else {
            out.push_back(static_cast<char>(c));
            space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> Paragraphs(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
        const auto para = Collapse(current);
        if (detail::WordCount(para) >= kMinParagraphWords) out.push_back(para);
        current.clear();
    };
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            // a blank line (newline, optional spaces, newline) ends the paragraph
            std::size_t j = i + 1;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r')) ++j;
            if (j < text.size() && text[j] == '\n') {
                flush();
                i = j + 1;
                continue;
            }
        }
        current.push_back(text[i]);
        ++i;
    }
    flush();
    return out;
}

// The leading dotted number of a paragraph ("1.2.3"), or empty
std::string LeadingNumber(const std::string& paragraph) {
    std::size_t i = 0;
    while (i < paragraph.size() && std::isdigit(static_cast<unsigned char>(paragraph[i]))) ++i;
    if (i == 0 || i >= paragraph.size() || paragraph[i] != '.') return "";
    std::size_t end = i;
    while (end < paragraph.size() && paragraph[end] == '.') {
        std::size_t k = end + 1;
        while (k < paragraph.size() && std::isdigit(static_cast<unsigned char>(paragraph[k]))) ++k;
        if (k == end + 1) break;
        end = k;
    }
    if (end < paragraph.size() && std::isalnum(static_cast<unsigned char>(paragraph[end])))
        return "";
    return paragraph.substr(0, end);
}

// A string field that may be absent or null
std::string Str(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool IncludeDocument(const nlohmann::json& entry, const std::string& code,
                     const std::set<std::string>& requested_codes) {
    if (!requested_codes.count(code)) return false;
    const auto stub = entry.find("is_stub");
    if (stub != entry.end() && stub->is_boolean() && stub->get<bool>()) return false;
    return Str(entry, "status") == "current";
}

std::vector<Chunk> ChunksFromDocument(const nlohmann::json& doc, std::set<std::string>& seen) {
    std::vector<Chunk> out;
    const auto code = doc.at("code").get<std::string>();
    const auto title = Str(doc, "title");
    const auto source_url = Str(doc, "source_url");
    const auto last_updated = Str(doc, "last_updated");
    for (const auto& chapter : doc.at("chapters")) {
        const auto chapter_title = Str(chapter, "title");
        const auto slug = Str(chapter, "slug");
        for (const auto& rec : chapter.at("recommendations")) {
            if (Str(rec, "kind") != "recommendation") continue;
            const auto id = rec.at("id").get<std::string>();
            if (!seen.insert(id).second) continue;
            Chunk chunk;
            chunk.id = id;
            chunk.code = code;
            chunk.title = title;
            chunk.chapter = chapter_title;
            chunk.number = Str(rec, "number");
            chunk.section = Str(rec, "section");
            chunk.update_tag = Str(rec, "update_tag");
            chunk.last_updated = last_updated;
            chunk.text = std::string(detail::Trim(Str(rec, "text")));
            chunk.url = source_url + "/chapter/" + slug + "#" + id;
            chunk.source = "nice";
            out.push_back(std::move(chunk));
        }
    }
    return out;
}

bool StartsRecommendation(const std::string& paragraph) {
    if (!LeadingNumber(paragraph).empty()) return true;
    static constexpr std::string_view kWord = "recommendation";
    if (paragraph.size() <= kWord.size()) return false;
    for (std::size_t i = 0; i < kWord.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(paragraph[i])) != kWord[i]) return false;
    }
    std::size_t i = kWord.size();
    if (!std::isspace(static_cast<unsigned char>(paragraph[i]))) return false;
    while (i < paragraph.size() && std::isspace(static_cast<unsigned char>(paragraph[i]))) ++i;
    std::size_t digits = i;
    while (digits < paragraph.size() &&
           std::isdigit(static_cast<unsigned char>(paragraph[digits]))) {
        ++digits;
    }
    if (digits == i) return false;
    if (digits < paragraph.size() && std::isalpha(static_cast<unsigned char>(paragraph[digits]))) {
        ++digits;
    }
    return digits >= paragraph.size() ||
           !std::isalnum(static_cast<unsigned char>(paragraph[digits]));
}

std::vector<Chunk> ChunksFromText(const std::string& code, const std::string& title,
                                  const std::string& text, const std::string& url) {
    std::vector<Chunk> out;
    std::vector<std::string> buffer;
    int buffered = 0;
    std::string number;
    auto close = [&] {
        if (buffer.empty()) return;
        Chunk chunk;
        chunk.id = code + "-" + std::to_string(out.size() + 1);
        chunk.code = code;
        chunk.title = title;
        chunk.number = number;
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            if (i) chunk.text += ' ';
            chunk.text += buffer[i];
        }
        chunk.url = url;
        chunk.source = "text";
        out.push_back(std::move(chunk));
        buffer.clear();
        buffered = 0;
        number.clear();
    };
    for (const auto& para : Paragraphs(text)) {
        const int words = detail::WordCount(para);
        const bool starts = StartsRecommendation(para);
        if (!buffer.empty() &&
            (starts || buffered + words > kMaxWords || buffered >= kTargetWords)) {
            close();
        }
        if (buffer.empty()) number = LeadingNumber(para);
        buffer.push_back(para);
        buffered += words;
    }
    close();
    return out;
}

}  // namespace ambient::guidance
