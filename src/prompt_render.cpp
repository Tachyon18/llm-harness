// prompt_render.cpp — prompt_render.h 구현
#include "prompt_render.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace prompt {

    namespace fs = std::filesystem;

    namespace {

        std::string Trim(const std::string& s) {
            const auto b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return "";
            const auto e = s.find_last_not_of(" \t");
            return s.substr(b, e - b + 1);
        }

        std::string AbsForMessage(const std::string& path) {
            std::error_code ec;
            const fs::path p = fs::absolute(fs::path(path), ec);
            return ec ? path : p.string();
        }

        // "npc.taboo.topic" → ctx["npc"]["taboo"]["topic"]. 없으면 nullptr
        const json* Lookup(const json& ctx, const std::string& key) {
            const json* cur = &ctx;
            size_t start = 0;
            while (start <= key.size()) {
                const size_t dot = key.find('.', start);
                const std::string part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                if (part.empty() || !cur->is_object()) return nullptr;
                auto it = cur->find(part);
                if (it == cur->end()) return nullptr;
                cur = &*it;
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
            return cur;
        }

        // 값 → 템플릿에 들어갈 문자열. 지원하지 않는 형태면 false
        bool ValueToText(const json& v, std::string& out, std::string& why) {
            if (v.is_string()) { out = v.get<std::string>(); return true; }
            if (v.is_number() || v.is_boolean()) { out = v.dump(); return true; }
            if (v.is_array()) {
                if (v.empty()) { out = "- (없음)"; return true; }
                std::string r;
                for (size_t i = 0; i < v.size(); ++i) {
                    if (!v[i].is_string()) {
                        why = "array element " + std::to_string(i) + " is not a string";
                        return false;
                    }
                    if (i) r += '\n';
                    r += "- " + v[i].get<std::string>();
                }
                out = r;
                return true;
            }
            why = v.is_null() ? "value is null" : "objects cannot be inserted directly (use a deeper key)";
            return false;
        }

        std::vector<std::string> StringArray(const json& obj, const char* key) {
            std::vector<std::string> r;
            if (!obj.is_object()) return r;
            auto it = obj.find(key);
            if (it == obj.end() || !it->is_array()) return r;
            for (const auto& e : *it)
                if (e.is_string() && !e.get<std::string>().empty()) r.push_back(e.get<std::string>());
            return r;
        }

    }  // namespace

    std::optional<std::string> ReadTextFile(const std::string& path, std::string& err) {
        std::ifstream in(fs::path(path), std::ios::binary);
        if (!in) {
            err = "cannot open: " + AbsForMessage(path);
            return std::nullopt;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }
        // UTF-8 검사: CP949(ANSI)로 저장된 파일이면 한글이 조용히 깨지므로 여기서 멈춘다
        for (size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            bool ok = len > 0 && i + len <= text.size();
            for (size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(text[i + k]) >> 6) == 0x2;
            if (!ok) {
                err = "not valid UTF-8 at byte " + std::to_string(i) + ": " + AbsForMessage(path) +
                    " (saved as ANSI/CP949? re-save as UTF-8)";
                return std::nullopt;
            }
            i += len;
        }
        // 메모장 저장(CRLF)이어도 프롬프트 토큰이 달라지지 않게 LF로 통일
        std::string norm;
        norm.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r') {
                norm += '\n';
                if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            }
            else {
                norm += text[i];
            }
        }
        return norm;
    }

    std::optional<json> ReadJsonFile(const std::string& path, std::string& err) {
        auto text = ReadTextFile(path, err);
        if (!text) return std::nullopt;
        try {
            return json::parse(*text);
        }
        catch (const std::exception& e) {
            err = "JSON parse error in " + path + ": " + e.what();
            return std::nullopt;
        }
    }

    bool RenderTemplate(const std::string& tpl, const json& ctx, std::string& out, std::string& err,
        std::vector<std::string>* usedKeys, int seed, json* picks) {
        out.clear();
        size_t pos = 0;
        auto lineOf = [&](size_t p) { return 1 + std::count(tpl.begin(), tpl.begin() + static_cast<std::ptrdiff_t>(p), '\n'); };
        while (true) {
            const size_t open = tpl.find("{{", pos);
            if (open == std::string::npos) {
                out.append(tpl, pos, std::string::npos);
                break;
            }
            out.append(tpl, pos, open - pos);
            const size_t close = tpl.find("}}", open + 2);
            if (close == std::string::npos) {
                err = "line " + std::to_string(lineOf(open)) + ": '{{' without '}}'";
                return false;
            }
            const std::string raw = Trim(tpl.substr(open + 2, close - open - 2));
            if (raw.empty() || raw.find('\n') != std::string::npos || raw.find('{') != std::string::npos) {
                err = "line " + std::to_string(lineOf(open)) + ": bad key '{{" + raw + "}}'";
                return false;
            }
            // (v0.3) 필터: "키|pick"
            std::string key = raw, filter;
            if (const size_t bar = raw.find('|'); bar != std::string::npos) {
                key = Trim(raw.substr(0, bar));
                filter = Trim(raw.substr(bar + 1));
                if (filter != "pick") {
                    err = "line " + std::to_string(lineOf(open)) + ": unknown filter '|" + filter + "' (only |pick)";
                    return false;
                }
            }
            const json* v = Lookup(ctx, key);
            if (!v) {
                err = "line " + std::to_string(lineOf(open)) + ": no data for '{{" + key + "}}'";
                return false;
            }
            std::string text, why;
            if (filter == "pick") {
                if (!v->is_array() || v->empty()) {
                    err = "line " + std::to_string(lineOf(open)) + ": '{{" + raw + "}}' needs a non-empty array";
                    return false;
                }
                const long long n = static_cast<long long>(v->size());
                const long long idx = ((static_cast<long long>(seed) - 1) % n + n) % n;
                const json& e = (*v)[static_cast<size_t>(idx)];
                if (!e.is_string()) {
                    err = "line " + std::to_string(lineOf(open)) + ": '{{" + raw + "}}' element is not a string";
                    return false;
                }
                text = e.get<std::string>();
                if (picks) picks->push_back({ {"key", key}, {"index", idx}, {"count", n} });
            }
            else if (!ValueToText(*v, text, why)) {
                err = "line " + std::to_string(lineOf(open)) + ": '{{" + key + "}}' " + why;
                return false;
            }
            out += text;
            if (usedKeys && std::find(usedKeys->begin(), usedKeys->end(), raw) == usedKeys->end()) usedKeys->push_back(raw);
            pos = close + 2;
        }
        // 끝의 빈 줄 정리 (템플릿 파일 끝 줄바꿈이 토큰으로 들어가지 않게)
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        return true;
    }

    bool RenderNpcPrompt(NpcPrompt& p, int seed, std::string& err) {
        std::string out, renderErr;
        std::vector<std::string> used;
        json picks = json::array();
        if (!RenderTemplate(p.templateText, p.ctx, out, renderErr, &used, seed, &picks)) {
            err = p.templatePath + ": " + renderErr;
            return false;
        }
        p.system = std::move(out);
        p.usedKeys = std::move(used);
        p.picks = std::move(picks);
        p.seed = seed;
        return true;
    }

    std::optional<NpcPrompt> BuildNpcPrompt(const std::string& worldPath, const std::string& npcPath,
        const std::string& templatePath, std::string& err, int seed) {
        auto world = ReadJsonFile(worldPath, err);
        if (!world) return std::nullopt;
        auto npc = ReadJsonFile(npcPath, err);
        if (!npc) return std::nullopt;
        auto tpl = ReadTextFile(templatePath, err);
        if (!tpl) return std::nullopt;
        if (!world->is_object() || !npc->is_object()) {
            err = "world/npc file must be a JSON object";
            return std::nullopt;
        }

        NpcPrompt p;
        p.worldPath = worldPath;
        p.npcPath = npcPath;
        p.templatePath = templatePath;
        p.templateTag = fs::path(templatePath).stem().string();

        // 레벨 → world.dialogue_levels["<레벨>"]
        if (!npc->contains("level") || !(*npc)["level"].is_number_integer()) {
            err = npcPath + ": \"level\" must be an integer (1~3)";
            return std::nullopt;
        }
        p.level = (*npc)["level"].get<int>();
        const json* levelObj = Lookup(*world, "dialogue_levels." + std::to_string(p.level));
        if (!levelObj || !levelObj->is_object()) {
            err = worldPath + ": no dialogue_levels.\"" + std::to_string(p.level) + "\" for npc level";
            return std::nullopt;
        }
        if (levelObj->contains("max_tokens") && (*levelObj)["max_tokens"].is_number_integer())
            p.levelMaxTokens = (*levelObj)["max_tokens"].get<int>();
        if (levelObj->contains("sentences") && (*levelObj)["sentences"].is_array() && (*levelObj)["sentences"].size() == 2 &&
            (*levelObj)["sentences"][0].is_number_integer() && (*levelObj)["sentences"][1].is_number_integer()) {
            p.checks.minSentences = (*levelObj)["sentences"][0].get<int>();
            p.checks.maxSentences = (*levelObj)["sentences"][1].get<int>();
        }

        p.ctx = { {"world", *world}, {"npc", *npc}, {"level", *levelObj} };
        p.templateText = *tpl;
        if (!RenderNpcPrompt(p, seed, err)) return std::nullopt;

        // 점검 설정 (eval 블록은 템플릿에 넣지 않는 평가 전용 데이터)
        p.checks.npcName = npc->value("name", "");
        const json evalObj = npc->value("eval", json::object());
        p.checks.tabooKeywords = StringArray(evalObj, "taboo_keywords");
        p.checks.forbiddenWords = StringArray(*world, "forbidden_words");
        for (auto& w : StringArray(evalObj, "forbidden_words")) p.checks.forbiddenWords.push_back(w);
        // (v0.3) 마을 사람 이름: "보리스: 대장장이..." → "보리스". 자기 자신은 제외
        for (const auto& r : StringArray(*world, "residents")) {
            const size_t colon = r.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = Trim(r.substr(0, colon));
            if (!name.empty() && name != p.checks.npcName) p.checks.residentNames.push_back(name);
        }
        return p;
    }

}  // namespace prompt