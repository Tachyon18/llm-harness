// reply_checks.cpp — reply_checks.h 구현
#include "reply_checks.h"

namespace checks {

    namespace {

        // UTF-8 → 코드 포인트. 잘못된 바이트는 U+FFFD로
        std::u32string Decode(const std::string& s) {
            std::u32string r;
            size_t i = 0;
            while (i < s.size()) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                char32_t cp = 0xFFFD;
                size_t len = 1;
                if (c < 0x80) { cp = c; }
                else if ((c >> 5) == 0x6) { len = 2; cp = c & 0x1Fu; }
                else if ((c >> 4) == 0xE) { len = 3; cp = c & 0x0Fu; }
                else if ((c >> 3) == 0x1E) { len = 4; cp = c & 0x07u; }
                else { r.push_back(0xFFFD); ++i; continue; }
                if (i + len > s.size()) { r.push_back(0xFFFD); break; }
                bool ok = true;
                for (size_t k = 1; k < len; ++k) {
                    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc >> 6) != 0x2) { ok = false; break; }
                    cp = (cp << 6) | (cc & 0x3Fu);
                }
                r.push_back(ok ? cp : 0xFFFD);
                i += ok ? len : 1;
            }
            return r;
        }

        bool IsEmoji(char32_t c) {
            return (c >= 0x1F000 && c <= 0x1FAFF)   // 이모티콘, 그림 문자, 기호 확장
                || (c >= 0x2600 && c <= 0x27BF)     // 기타 기호(☀♥♪), 딩뱃(✨✔)
                || (c >= 0x2B00 && c <= 0x2BFF)     // ⭐ 등
                || c == 0xFE0F || c == 0x200D;      // 이모지 변형 선택자, 결합자
        }

        bool IsHangul(char32_t c) {
            return (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
        }

        bool IsLatin(char32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
        bool IsLetterLike(char32_t c) { return IsHangul(c) || IsLatin(c) || (c >= '0' && c <= '9') || (c >= 0x4E00 && c <= 0x9FFF); }
        bool IsSpace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x3000; }
        bool IsQuote(char32_t c) { return c == '"' || c == '\'' || c == 0x201C || c == 0x201D || c == 0x2018 || c == 0x2019 || c == 0x300C || c == 0x300D; }

    }  // namespace

    int CountSentences(const std::string& text) {
        const std::u32string u = Decode(text);
        int count = 0;
        bool hasLetter = false;
        auto boundary = [&] {
            if (hasLetter) ++count;
            hasLetter = false;
            };
        // 뒤에 공백·따옴표·닫는 괄호만 남았는지
        auto onlyTrailing = [&](size_t from) {
            for (size_t k = from; k < u.size(); ++k)
                if (!IsSpace(u[k]) && !IsQuote(u[k]) && u[k] != ')' && u[k] != '~') return false;
            return true;
            };
        for (size_t i = 0; i < u.size(); ++i) {
            const char32_t c = u[i];
            if (c == '\n') { boundary(); continue; }
            if (c == '?' || c == '!' || c == 0xFF1F || c == 0xFF01) {
                while (i + 1 < u.size() && (u[i + 1] == '?' || u[i + 1] == '!' || u[i + 1] == '.' || u[i + 1] == '~')) ++i;
                boundary();
                continue;
            }
            if (c == '.' || c == 0x2026) {
                size_t j = i;
                size_t dots = 0;
                while (j < u.size() && (u[j] == '.' || u[j] == 0x2026)) { dots += (u[j] == 0x2026) ? size_t{ 3 } : size_t{ 1 }; ++j; }
                i = j - 1;
                // 마침표 하나 = 문장 끝. 말줄임은 텍스트 끝일 때만 문장 끝
                if (dots == 1 || onlyTrailing(j)) boundary();
                continue;
            }
            if (IsLetterLike(c)) hasLetter = true;
        }
        boundary();
        return count;
    }

    ReplyCheck CheckReply(const std::string& text, const prompt::CheckConfig& cfg) {
        ReplyCheck r;
        for (const auto& k : cfg.tabooKeywords)
            if (text.find(k) != std::string::npos) r.tabooHits.push_back(k);
        for (const auto& w : cfg.forbiddenWords)
            if (text.find(w) != std::string::npos) r.forbiddenHits.push_back(w);
        for (const auto& n : cfg.residentNames)
            if (text.find(n) != std::string::npos) r.residentHits.push_back(n);

        const std::u32string u = Decode(text);
        for (char32_t c : u) {
            if (c == '(' || c == 0xFF08 || c == '*' || c == '[') ++r.stageMarks;
            if (IsEmoji(c)) ++r.emoji;
            if (IsLatin(c)) ++r.latin;
        }

        size_t first = 0;
        while (first < u.size() && IsSpace(u[first])) ++first;
        if (first < u.size() && IsQuote(u[first])) r.quoted = true;

        if (!cfg.npcName.empty()) {
            size_t s = first;
            while (s < u.size() && (IsQuote(u[s]) || u[s] == '*' || IsSpace(u[s]))) ++s;
            const std::u32string name = Decode(cfg.npcName);
            if (u.compare(s, name.size(), name) == 0) {
                size_t k = s + name.size();
                while (k < u.size() && (u[k] == ' ' || u[k] == '*')) ++k;
                if (k < u.size() && (u[k] == ':' || u[k] == 0xFF1A)) r.namePrefix = true;
            }
        }

        r.sentences = CountSentences(text);
        r.sentenceRangeMin = cfg.minSentences;
        r.sentenceRangeMax = cfg.maxSentences;
        return r;
    }

    nlohmann::json ReplyCheck::ToJson() const {
        nlohmann::json j = {
            {"taboo_hits", tabooHits},
            {"forbidden_hits", forbiddenHits},
            {"resident_hits", residentHits},
            {"stage_marks", stageMarks},
            {"emoji", emoji},
            {"latin_chars", latin},
            {"name_prefix", namePrefix},
            {"quoted", quoted},
            {"sentences", sentences},
        };
        if (sentenceRangeMax > 0) {
            j["sentence_range"] = { sentenceRangeMin, sentenceRangeMax };
            j["sentences_over"] = SentencesOver();
            j["sentences_under"] = SentencesUnder();
        }
        return j;
    }

    std::string ReplyCheck::Flags() const {
        std::string f;
        auto add = [&](const std::string& s) { f += (f.empty() ? "" : " ") + s; };
        if (!tabooHits.empty()) {
            std::string k;
            for (const auto& h : tabooHits) k += (k.empty() ? "" : ",") + h;
            add("TABOO[" + k + "]");
        }
        if (!forbiddenHits.empty()) {
            std::string k;
            for (const auto& h : forbiddenHits) k += (k.empty() ? "" : ",") + h;
            add("FORBIDDEN[" + k + "]");
        }
        if (!residentHits.empty()) {
            std::string k;
            for (const auto& h : residentHits) k += (k.empty() ? "" : ",") + h;
            add("names[" + k + "]");   // 소문자: 문제 표시가 아니라 정보
        }
        if (stageMarks > 0) add("STAGE(" + std::to_string(stageMarks) + ")");
        if (emoji > 0) add("EMOJI(" + std::to_string(emoji) + ")");
        if (latin > 0) add("LATIN(" + std::to_string(latin) + ")");
        if (namePrefix) add("NAME:");
        if (quoted) add("QUOTED");
        if (SentencesOver()) add("LONG(" + std::to_string(sentences) + " sent > " + std::to_string(sentenceRangeMax) + ")");
        if (SentencesUnder()) add("SHORT(" + std::to_string(sentences) + " sent < " + std::to_string(sentenceRangeMin) + ")");
        return f;
    }

}  // namespace checks