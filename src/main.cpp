// llm-harness v0.3 — LLM-GM 단계 1 테스트 하네스
//
// 시나리오(JSON) 하나를 읽어 llama-server의 /v1/chat/completions에 여러 턴으로 보내고,
// 턴마다 응답·토큰 수·시간·자동 점검 결과를 results/ 아래 JSONL 파일로 기록한다.
//
// 사용법:
//   llm-harness <scenario.json> [옵션]
//   llm-harness --health                      서버 상태만 확인
//   llm-harness <scenario.json> --render      추론 없이 렌더링된 시스템 프롬프트 출력·저장 (+토큰 수)
//   llm-harness <scenario.json> --tokenize    추론 없이 템플릿 적용 결과와 토큰 수만 조사
//
// 옵션:
//   --host <주소>        기본 127.0.0.1
//   --port <번호>        기본 8081
//   --out <폴더>         결과 폴더, 기본 results
//   --wait <초>          서버가 "모델 로딩 중"(503)이면 최대 이 시간까지 기다림, 기본 120
//   --repeat <N>         같은 시나리오를 N회 연속 실행 (회차마다 대화는 새로 시작), 기본 1
//   --label <태그>       결과 파일 이름에 붙일 태그 (예: cold, warm, ctx2k)
//   --template <파일>    NPC 시나리오의 템플릿을 바꿔서 실행 (금기 변형 A/B/C 비교용)
//   --npc <파일>         NPC 시나리오의 NPC 데이터를 바꿔서 실행 (공통 시나리오를 NPC마다 쓰기)
//   --seed <N>           템플릿 |pick 시작 값, 기본 1. 회차 r은 seed N+r-1 (예시 대사를 회차마다 돌려 씀)
//   --render             렌더링 결과만 확인. 서버가 켜져 있으면 토큰 수도 출력, 꺼져 있어도 동작
//   --tokenize           /apply-template + /tokenize로 system+첫 발화의 토큰 구성을 저장 (추론 없음)
//   --save-raw           턴마다 서버 응답 원문 전체를 JSONL에 함께 저장
//   --print              응답 텍스트(및 reasoning)를 콘솔에도 출력
//
// 시나리오 형식 (둘 중 하나):
//   (기존) "system": "시스템 프롬프트 문자열"
//   (v0.2) "world": "world/mistvale.json", "npc": "npcs/marta.json", "template": "prompts/npc_v1_A.txt"
//          경로는 실행 폴더(하네스 루트) 기준. 이 방식이면 params에 max_tokens가 없을 때
//          NPC 레벨의 하드 캡(world.dialogue_levels.<레벨>.max_tokens)을 넣는다.
//
// 종료 코드: 0 성공 / 1 사용법 오류 / 2 서버 연결 실패 / 3 서버 준비 안 됨
//            4 시나리오 오류 / 5 요청 처리 중 오류
//
// 출력 JSONL 줄 종류 ("type" 필드):
//   run          실행 정보 (시나리오, 모델 id, 시스템 프롬프트 전문, 프롬프트 출처, 요청 옵션, 반복 횟수)
//   turn         턴 결과 (rep, 사용자 발화, 응답, checks, reasoning, finish_reason, 왕복 ms, timings, usage)
//   error        실패한 턴 (kind: connection / http / parse)
//   summary      회차별 요약 (점검 항목별 해당 턴 수 포함)
//   summary_all  전체 요약 + 턴 번호별 집계 by_turn (--repeat 2 이상일 때만)
//
// v0.1 → v0.2 변경:
//   - world + npc + template 렌더링 (prompt_render.*), --render, --template
//   - 레벨별 max_tokens 하드 캡 자동 설정 (시나리오 params 값이 우선)
//   - 턴별 자동 점검 (reply_checks.*): 금기 키워드, 금지어, 지문 기호, 이모지, 영문, 이름표, 따옴표,
//     문장 수(레벨 범위), finish_reason=length → 요약에 비율, 반복 시 턴 번호별 집계
//   - run 줄에 시스템 프롬프트 전문 기록, NPC 시나리오는 결과 파일 이름에 템플릿 이름 포함
//
// v0.2 → v0.3 변경:
//   - 템플릿 필터 {{키|pick}}: 배열에서 하나를 골라 넣음. 회차마다 seed가 바뀌어 다른 예시가 들어감
//     (회차별 시스템 프롬프트는 summary 줄의 system/picks에 기록. run 줄의 system은 1회차 것)
//   - --npc, --seed. 시나리오 이름에 NPC 파일 이름이 없으면 결과 파일 이름에 NPC 이름 추가
//   - 자동 점검에 다른 마을 사람 이름 언급(names) 추가 — 실패가 아니라 "엮어서 지어내기" 검토용 표시

#include "prompt_render.h"
#include "reply_checks.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

    constexpr const char* kHarnessVersion = "v0.3";

    enum ExitCode : int {
        kOk = 0,
        kUsage = 1,
        kConnectFail = 2,
        kNotReady = 3,
        kScenarioError = 4,
        kRequestError = 5,
    };

    struct Options {
        std::string scenarioPath;
        std::string host = "127.0.0.1";
        int port = 8081;
        std::string outDir = "results";
        int waitSeconds = 120;
        int repeat = 1;
        std::string label;
        std::string templateOverride;
        std::string npcOverride;
        int seed = 1;
        bool healthOnly = false;
        bool render = false;
        bool tokenize = false;
        bool saveRaw = false;
        bool print = false;
    };

    struct Scenario {
        std::string name;
        std::string system;
        std::vector<std::string> turns;
        json params = json::object();   // 요청 본문에 그대로 덧붙일 옵션 (max_tokens, temperature 등)
        std::optional<prompt::NpcPrompt> npc;   // world/npc/template 방식일 때만
        std::string maxTokensSource = "none";   // "scenario" / "level" / "none"
        prompt::CheckConfig checks;             // system 방식이면 기본값(키워드 없음)
    };

    struct ModelInfo {
        std::string id = "unknown";     // 서버가 알려 준 모델 id 원문
        std::string tag = "unknown";    // 파일 이름용으로 정리한 것
        json raw;                       // /v1/models 응답 원문 (필드 확인용)
    };

    // 점검 항목별 "해당 턴 수"
    struct CheckCounts {
        int taboo = 0, forbidden = 0, stage = 0, emoji = 0, latin = 0, namePrefix = 0, quoted = 0, residents = 0;
        int sentOver = 0, sentUnder = 0, lengthCut = 0;
        long long sentences = 0;
        int maxPredicted = 0;

        void Add(const checks::ReplyCheck& c, bool cut, int predN) {
            taboo += !c.tabooHits.empty();
            forbidden += !c.forbiddenHits.empty();
            residents += !c.residentHits.empty();
            stage += c.stageMarks > 0;
            emoji += c.emoji > 0;
            latin += c.latin > 0;
            namePrefix += c.namePrefix;
            quoted += c.quoted;
            sentOver += c.SentencesOver();
            sentUnder += c.SentencesUnder();
            lengthCut += cut;
            sentences += c.sentences;
            maxPredicted = std::max(maxPredicted, predN);
        }
        void Merge(const CheckCounts& o) {
            taboo += o.taboo; forbidden += o.forbidden; residents += o.residents; stage += o.stage; emoji += o.emoji; latin += o.latin;
            namePrefix += o.namePrefix; quoted += o.quoted; sentOver += o.sentOver; sentUnder += o.sentUnder;
            lengthCut += o.lengthCut; sentences += o.sentences; maxPredicted = std::max(maxPredicted, o.maxPredicted);
        }
        json ToJson(int turns) const {
            return {
                {"turns", turns},
                {"taboo_turns", taboo},
                {"forbidden_turns", forbidden},
                {"resident_mention_turns", residents},
                {"stage_mark_turns", stage},
                {"emoji_turns", emoji},
                {"latin_turns", latin},
                {"name_prefix_turns", namePrefix},
                {"quoted_turns", quoted},
                {"sentences_over_turns", sentOver},
                {"sentences_under_turns", sentUnder},
                {"finish_length_turns", lengthCut},
                {"avg_sentences", turns > 0 ? static_cast<double>(sentences) / turns : 0.0},
                {"max_predicted_tokens", maxPredicted},
            };
        }
    };

    // 회차(rep) 하나의 집계
    struct RunStats {
        int completed = 0;
        long long predictedTokens = 0;
        double predictedMs = 0.0;       // 서버가 잰 생성 시간 합
        long long promptTokens = 0;     // 새로 처리한 입력 토큰 합 (timings.prompt_n)
        long long cachedTokens = 0;     // 캐시에서 재사용한 입력 토큰 합 (timings.cache_n)
        double promptMs = 0.0;          // 서버가 잰 입력 처리 시간 합
        double wallMs = 0.0;            // 하네스가 잰 왕복 시간 합
        int emptyContentTurns = 0;
        int reasoningTurns = 0;
        CheckCounts checks;
        int exitCode = kOk;

        double GenTpsServer() const { return predictedMs > 0 ? static_cast<double>(predictedTokens) * 1000.0 / predictedMs : 0.0; }
        double GenTpsWall() const { return wallMs > 0 ? static_cast<double>(predictedTokens) * 1000.0 / wallMs : 0.0; }
    };

    // 턴 번호별 집계 (반복 실행에서 "4턴 금기 3/10" 같은 값을 보기 위해)
    struct TurnAgg {
        int n = 0;
        long long predicted = 0;
        CheckCounts checks;
    };

    // ---------------------------------------------------------------------------
    // 공용 도구
    // ---------------------------------------------------------------------------

    std::string NowStamp(const char* format) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), format, &tm);
        return buf;
    }

    // 잘못된 UTF-8이 섞여 있어도 예외 대신 대체 문자로 직렬화
    std::string Dump(const json& j, int indent = -1) {
        return j.dump(indent, ' ', false, json::error_handler_t::replace);
    }

    // 키가 없거나 문자열이 아니면(null 포함) 빈 문자열
    std::string GetStr(const json& obj, const char* key) {
        if (!obj.is_object()) return "";
        auto it = obj.find(key);
        if (it == obj.end() || !it->is_string()) return "";
        return it->get<std::string>();
    }

    template <typename T>
    T GetNum(const json& obj, const char* key, T fallback) {
        if (!obj.is_object()) return fallback;
        auto it = obj.find(key);
        if (it == obj.end() || !it->is_number()) return fallback;
        return it->get<T>();
    }

    // 파일 이름에 쓸 수 있는 문자만 남긴다 (영문·숫자·.-_ 이외는 '-')
    std::string SanitizeForFile(const std::string& s, size_t maxLen = 40) {
        std::string r;
        for (char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (std::isalnum(c) || c == '.' || c == '-' || c == '_') r += static_cast<char>(c);
            else r += '-';
        }
        if (r.size() > maxLen) r.resize(maxLen);
        while (!r.empty() && (r.back() == '-' || r.back() == '.')) r.pop_back();
        return r.empty() ? "unknown" : r;
    }

    // 모델 id → 파일 이름 태그.
    //   "google/gemma-4-E4B-it-qat-q4_0-gguf" → "gemma-4-E4B-it-qat-q4_0-gguf"
    //   "unsloth/Qwen3.5-4B-GGUF:Q4_0"        → "Qwen3.5-4B-GGUF-Q4_0"
    //   "C:\...\model.gguf"                   → "model"
    std::string ModelTagFromId(std::string id) {
        const auto slash = id.find_last_of("/\\");
        if (slash != std::string::npos) id = id.substr(slash + 1);
        if (id.size() > 5 && id.compare(id.size() - 5, 5, ".gguf") == 0) id.resize(id.size() - 5);
        return SanitizeForFile(id);
    }

    void PrintUsage() {
        std::cerr << "usage: llm-harness <scenario.json> [--host H] [--port P] [--out DIR] [--wait SEC]\n"
            "                    [--repeat N] [--label TAG] [--template FILE] [--npc FILE] [--seed N]\n"
            "                    [--save-raw] [--print]\n"
            "       llm-harness <scenario.json> --render [--template FILE] [--npc FILE] [--seed N] [--host H] [--port P]\n"
            "       llm-harness <scenario.json> --tokenize [--template FILE] [--host H] [--port P] [--out DIR]\n"
            "       llm-harness --health [--host H] [--port P] [--wait SEC]\n";
    }

    std::optional<Options> ParseArgs(int argc, char** argv) {
        Options opt;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&](const char* name) -> std::optional<std::string> {
                if (i + 1 >= argc) {
                    std::cerr << name << " needs a value\n";
                    return std::nullopt;
                }
                return std::string(argv[++i]);
                };
            try {
                if (a == "--host") { auto v = next("--host"); if (!v) return std::nullopt; opt.host = *v; }
                else if (a == "--port") { auto v = next("--port"); if (!v) return std::nullopt; opt.port = std::stoi(*v); }
                else if (a == "--out") { auto v = next("--out"); if (!v) return std::nullopt; opt.outDir = *v; }
                else if (a == "--wait") { auto v = next("--wait"); if (!v) return std::nullopt; opt.waitSeconds = std::stoi(*v); }
                else if (a == "--repeat") { auto v = next("--repeat"); if (!v) return std::nullopt; opt.repeat = std::stoi(*v); }
                else if (a == "--label") { auto v = next("--label"); if (!v) return std::nullopt; opt.label = SanitizeForFile(*v, 24); }
                else if (a == "--template") { auto v = next("--template"); if (!v) return std::nullopt; opt.templateOverride = *v; }
                else if (a == "--npc") { auto v = next("--npc"); if (!v) return std::nullopt; opt.npcOverride = *v; }
                else if (a == "--seed") { auto v = next("--seed"); if (!v) return std::nullopt; opt.seed = std::stoi(*v); }
                else if (a == "--health") opt.healthOnly = true;
                else if (a == "--render") opt.render = true;
                else if (a == "--tokenize") opt.tokenize = true;
                else if (a == "--save-raw") opt.saveRaw = true;
                else if (a == "--print") opt.print = true;
                else if (!a.empty() && a[0] == '-') { std::cerr << "unknown option: " << a << "\n"; return std::nullopt; }
                else if (opt.scenarioPath.empty()) opt.scenarioPath = a;
                else { std::cerr << "only one scenario file is allowed\n"; return std::nullopt; }
            }
            catch (const std::exception&) {
                std::cerr << "invalid number for " << a << "\n";
                return std::nullopt;
            }
        }
        if (opt.repeat < 1) { std::cerr << "--repeat must be >= 1\n"; return std::nullopt; }
        if (opt.render && opt.tokenize) { std::cerr << "--render and --tokenize cannot be used together\n"; return std::nullopt; }
        if (!opt.healthOnly && opt.scenarioPath.empty()) return std::nullopt;
        return opt;
    }

    // ---------------------------------------------------------------------------
    // 서버 상태 확인: 200이면 준비 완료, 503이면 로딩 중이므로 기다림
    // ---------------------------------------------------------------------------

    int WaitForServer(httplib::Client& cli, int waitSeconds, bool quietConnectFail = false) {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(waitSeconds);
        bool announcedLoading = false;

        while (true) {
            auto res = cli.Get("/health");
            if (!res) {
                if (!quietConnectFail)
                    std::cerr << "[health] connection failed: " << httplib::to_string(res.error())
                    << "  (is llama-server running on this host/port?)\n";
                return kConnectFail;
            }
            if (res->status == 200) {
                std::cout << "[health] ready: " << res->body << "\n";
                return kOk;
            }
            if (res->status == 503) {
                if (!announcedLoading) {
                    std::cout << "[health] model loading (503), waiting up to " << waitSeconds << "s...\n";
                    announcedLoading = true;
                }
                if (clock::now() >= deadline) {
                    std::cerr << "[health] still not ready after " << waitSeconds << "s\n";
                    return kNotReady;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            std::cerr << "[health] unexpected status " << res->status << ": " << res->body << "\n";
            return kNotReady;
        }
    }

    // GET /v1/models → data[0].id. 실패해도 실행은 계속한다 (모델 이름은 "unknown").
    ModelInfo FetchModelInfo(httplib::Client& cli) {
        ModelInfo info;
        auto res = cli.Get("/v1/models");
        if (!res || res->status != 200) {
            std::cerr << "[model] /v1/models unavailable, using \"unknown\"\n";
            return info;
        }
        try {
            info.raw = json::parse(res->body);
            const auto& data = info.raw.at("data");
            if (data.is_array() && !data.empty()) {
                const std::string id = GetStr(data[0], "id");
                if (!id.empty()) {
                    info.id = id;
                    info.tag = ModelTagFromId(id);
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[model] cannot parse /v1/models: " << e.what() << "\n";
        }
        std::cout << "[model] " << info.id << "  (file tag: " << info.tag << ")\n";
        return info;
    }

    // ---------------------------------------------------------------------------
    // 시나리오 읽기
    // ---------------------------------------------------------------------------

    std::optional<Scenario> LoadScenario(const Options& opt) {
        std::string err;
        auto parsed = prompt::ReadJsonFile(opt.scenarioPath, err);
        if (!parsed) {
            std::cerr << "[scenario] " << err << "\n";
            return std::nullopt;
        }
        const json& j = *parsed;
        try {
            Scenario sc;
            sc.name = j.value("name", fs::path(opt.scenarioPath).stem().string());
            sc.system = j.value("system", "");
            if (!j.contains("turns") || !j["turns"].is_array() || j["turns"].empty()) {
                std::cerr << "[scenario] \"turns\" must be a non-empty array of strings\n";
                return std::nullopt;
            }
            for (const auto& t : j["turns"]) sc.turns.push_back(t.get<std::string>());
            if (j.contains("params")) {
                if (!j["params"].is_object()) {
                    std::cerr << "[scenario] \"params\" must be an object\n";
                    return std::nullopt;
                }
                sc.params = j["params"];
            }

            const bool npcMode = j.contains("npc") || j.contains("world") || j.contains("template");
            if (npcMode) {
                if (!sc.system.empty()) {
                    std::cerr << "[scenario] use either \"system\" or \"world\"+\"npc\"+\"template\", not both\n";
                    return std::nullopt;
                }
                const std::string worldPath = j.value("world", "");
                const std::string npcPath = opt.npcOverride.empty() ? j.value("npc", "") : opt.npcOverride;
                const std::string tplPath = opt.templateOverride.empty() ? j.value("template", "") : opt.templateOverride;
                if (worldPath.empty() || npcPath.empty() || tplPath.empty()) {
                    std::cerr << "[scenario] NPC scenario needs \"world\", \"npc\" and \"template\" (or --template)\n";
                    return std::nullopt;
                }
                auto np = prompt::BuildNpcPrompt(worldPath, npcPath, tplPath, err, opt.seed);
                if (!np) {
                    std::cerr << "[render] " << err << "\n";
                    return std::nullopt;
                }
                sc.system = np->system;
                sc.checks = np->checks;
                if (sc.params.contains("max_tokens")) sc.maxTokensSource = "scenario";
                else if (np->levelMaxTokens > 0) {
                    sc.params["max_tokens"] = np->levelMaxTokens;
                    sc.maxTokensSource = "level";
                }
                sc.npc = std::move(np);
            }
            else {
                if (!opt.templateOverride.empty() || !opt.npcOverride.empty()) {
                    std::cerr << "[scenario] --template/--npc need an NPC scenario (world/npc/template)\n";
                    return std::nullopt;
                }
                if (sc.params.contains("max_tokens")) sc.maxTokensSource = "scenario";
            }
            return sc;
        }
        catch (const std::exception& e) {
            std::cerr << "[scenario] parse error: " << e.what() << "\n";
            return std::nullopt;
        }
    }

    json PromptSourceJson(const Scenario& sc) {
        json src = { {"mode", sc.npc ? "npc" : "system"}, {"max_tokens_source", sc.maxTokensSource} };
        if (sc.npc) {
            src["world"] = sc.npc->worldPath;
            src["npc"] = sc.npc->npcPath;
            src["template"] = sc.npc->templatePath;
            src["level"] = sc.npc->level;
            src["used_keys"] = sc.npc->usedKeys;
            src["seed"] = sc.npc->seed;
            src["picks"] = sc.npc->picks;
        }
        return src;
    }

    json CheckConfigJson(const prompt::CheckConfig& c) {
        return { {"npc_name", c.npcName},
                {"taboo_keywords", c.tabooKeywords},
                {"forbidden_words", c.forbiddenWords},
                {"resident_names", c.residentNames},
                {"sentence_range", {c.minSentences, c.maxSentences}} };
    }

    // <out>/<시나리오>[_<템플릿>]_<모델>[_<라벨>][_<접미사>]_<YYYYMMDD-HHMMSS>.<확장자>
    fs::path MakeOutPath(const Options& opt, const Scenario& sc, const ModelInfo& model,
        const std::string& suffix, const std::string& ext) {
        std::string name = sc.name;
        if (sc.npc) {
            const std::string npcTag = SanitizeForFile(fs::path(sc.npc->npcPath).stem().string(), 24);
            if (sc.name.find(npcTag) == std::string::npos) name += "_" + npcTag;
            name += "_" + SanitizeForFile(sc.npc->templateTag, 24);
        }
        name += "_" + model.tag;
        if (!opt.label.empty()) name += "_" + opt.label;
        if (!suffix.empty()) name += "_" + suffix;
        name += "_" + NowStamp("%Y%m%d-%H%M%S") + ext;
        return fs::path(opt.outDir) / name;
    }

    // ---------------------------------------------------------------------------
    // 토큰 수 조사 (--tokenize, --render 공용)
    // ---------------------------------------------------------------------------

    // POST /tokenize → 토큰 배열 (with_pieces면 {id, piece} 객체 배열). 실패 시 nullopt
    std::optional<json> Tokenize(httplib::Client& cli, const std::string& text, bool withPieces) {
        json body = { {"content", text}, {"with_pieces", withPieces} };
        auto res = cli.Post("/tokenize", Dump(body), "application/json");
        if (!res) {
            std::cerr << "[tokenize] connection error: " << httplib::to_string(res.error()) << "\n";
            return std::nullopt;
        }
        if (res->status != 200) {
            std::cerr << "[tokenize] HTTP " << res->status << ": " << res->body << "\n";
            return std::nullopt;
        }
        try {
            json j = json::parse(res->body);
            if (!j.contains("tokens") || !j["tokens"].is_array()) {
                std::cerr << "[tokenize] response has no \"tokens\" array: " << res->body << "\n";
                return std::nullopt;
            }
            return j["tokens"];
        }
        catch (const std::exception& e) {
            std::cerr << "[tokenize] parse error: " << e.what() << "\n";
            return std::nullopt;
        }
    }

    struct TokenReport {
        std::string templatedPrompt;
        json fullTokens;
        size_t full = 0, system = 0, user = 0;
        long long overhead = 0;
    };

    // system + 첫 발화에 채팅 템플릿을 적용(/apply-template)하고 각 부분의 토큰 수를 센다
    std::optional<TokenReport> CountTokens(httplib::Client& cli, const Scenario& sc, json& bodyOut) {
        json messages = json::array();
        if (!sc.system.empty()) messages.push_back({ {"role", "system"}, {"content", sc.system} });
        messages.push_back({ {"role", "user"}, {"content", sc.turns.front()} });

        // 시나리오 params의 chat_template_kwargs(enable_thinking 등)도 함께 보낸다.
        // /apply-template이 이 필드를 반영하는지는 확인 필요 → 결과 문자열에서 직접 확인할 것.
        json body = { {"messages", messages} };
        if (sc.params.contains("chat_template_kwargs")) body["chat_template_kwargs"] = sc.params["chat_template_kwargs"];
        bodyOut = body;

        auto res = cli.Post("/apply-template", Dump(body), "application/json");
        if (!res) {
            std::cerr << "[apply-template] connection error: " << httplib::to_string(res.error()) << "\n";
            return std::nullopt;
        }
        if (res->status != 200) {
            std::cerr << "[apply-template] HTTP " << res->status << ": " << res->body << "\n";
            return std::nullopt;
        }
        TokenReport r;
        try {
            r.templatedPrompt = GetStr(json::parse(res->body), "prompt");
        }
        catch (const std::exception& e) {
            std::cerr << "[apply-template] parse error: " << e.what() << "\n";
            return std::nullopt;
        }
        if (r.templatedPrompt.empty()) {
            std::cerr << "[apply-template] no \"prompt\" string in response: " << res->body << "\n";
            return std::nullopt;
        }
        auto fullTokens = Tokenize(cli, r.templatedPrompt, true);
        auto systemTokens = Tokenize(cli, sc.system, false);
        auto userTokens = Tokenize(cli, sc.turns.front(), false);
        if (!fullTokens || !systemTokens || !userTokens) return std::nullopt;
        r.fullTokens = *fullTokens;
        r.full = fullTokens->size();
        r.system = systemTokens->size();
        r.user = userTokens->size();
        r.overhead = static_cast<long long>(r.full) - static_cast<long long>(r.system + r.user);
        return r;
    }

    std::string TokenLine(const TokenReport& t, size_t systemBytes) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "full prompt %zu tok = system %zu + user %zu + template %lld (system %zu bytes UTF-8)",
            t.full, t.system, t.user, t.overhead, systemBytes);
        return buf;
    }

    int RunTokenize(httplib::Client& cli, const Options& opt, const Scenario& sc, const ModelInfo& model) {
        json body;
        auto t = CountTokens(cli, sc, body);
        if (!t) return kRequestError;

        json result = {
            {"type", "tokenize"},
            {"harness", kHarnessVersion},
            {"scenario", sc.name},
            {"model", model.id},
            {"created_at", NowStamp("%Y-%m-%dT%H:%M:%S")},
            {"prompt_source", PromptSourceJson(sc)},
            {"chat_template_kwargs", body.value("chat_template_kwargs", json())},
            {"counts", {
                {"full_prompt", t->full},
                {"system_only", t->system},
                {"first_user_only", t->user},
                {"template_overhead", t->overhead},
                {"system_chars_utf8_bytes", sc.system.size()},
            }},
            {"prompt", t->templatedPrompt},
            {"full_tokens", t->fullTokens},
        };

        std::error_code ec;
        fs::create_directories(fs::path(opt.outDir), ec);
        const fs::path outPath = MakeOutPath(opt, sc, model, "tokenize", ".json");
        std::ofstream out(outPath, std::ios::binary);
        if (!out) {
            std::cerr << "[out] cannot create: " << outPath.string() << "\n";
            return kScenarioError;
        }
        out << Dump(result, 2) << "\n";
        std::cout << "[tokenize] " << TokenLine(*t, sc.system.size()) << "\n[tokenize] -> " << outPath.string() << "\n";
        if (opt.print) std::cout << "----- templated prompt -----\n" << t->templatedPrompt << "\n----------------------------\n";
        return kOk;
    }

    // ---------------------------------------------------------------------------
    // --render: 렌더링 결과 출력·저장. 서버는 있으면 토큰 수만 세고, 없어도 성공으로 끝낸다
    // ---------------------------------------------------------------------------

    int RunRender(httplib::Client& cli, const Options& opt, const Scenario& sc) {
        std::cout << "----- system prompt (" << sc.system.size() << " bytes UTF-8) -----\n"
            << sc.system << "\n--------------------------------------------\n";
        if (sc.npc) {
            std::cout << "[render] world " << sc.npc->worldPath << " | npc " << sc.npc->npcPath << " | template "
                << sc.npc->templatePath << "\n";
            std::cout << "[render] level " << sc.npc->level << ", sentences " << sc.checks.minSentences << "~"
                << sc.checks.maxSentences << "\n";
            std::cout << "[render] used keys (" << sc.npc->usedKeys.size() << "):";
            for (const auto& k : sc.npc->usedKeys) std::cout << " " << k;
            std::cout << "\n[render] seed " << sc.npc->seed << ", picks " << Dump(sc.npc->picks);
            std::cout << "\n[checks] taboo keywords:";
            for (const auto& k : sc.checks.tabooKeywords) std::cout << " " << k;
            std::cout << "\n[checks] forbidden words:";
            for (const auto& k : sc.checks.forbiddenWords) std::cout << " " << k;
            std::cout << "\n[checks] resident names:";
            for (const auto& k : sc.checks.residentNames) std::cout << " " << k;
            std::cout << "\n";
        }
        std::cout << "[render] max_tokens " << (sc.params.contains("max_tokens") ? Dump(sc.params["max_tokens"]) : "-")
            << " (source: " << sc.maxTokensSource << ")\n";

        ModelInfo model;
        std::optional<TokenReport> tokens;
        if (WaitForServer(cli, 0, true) == kOk) {
            model = FetchModelInfo(cli);
            json body;
            tokens = CountTokens(cli, sc, body);
            if (tokens) std::cout << "[render] " << TokenLine(*tokens, sc.system.size()) << "\n";
        }
        else {
            model.tag = "noserver";
            std::cout << "[render] server not reachable -> token count skipped\n";
        }

        std::error_code ec;
        fs::create_directories(fs::path(opt.outDir), ec);
        const fs::path outPath = MakeOutPath(opt, sc, model, "render", ".txt");
        std::ofstream out(outPath, std::ios::binary);
        if (!out) {
            std::cerr << "[out] cannot create: " << outPath.string() << "\n";
            return kScenarioError;
        }
        out << sc.system << "\n";   // 시스템 프롬프트 원문만 (A/B/C 파일 비교용)
        std::cout << "[render] -> " << outPath.string() << "\n";
        return kOk;
    }

    // ---------------------------------------------------------------------------
    // 시나리오 1회 실행 (rep = 1부터)
    // ---------------------------------------------------------------------------

    template <typename WriteLine>
    RunStats RunOnce(httplib::Client& cli, const Options& opt, const Scenario& sc, int rep, WriteLine& writeLine,
        std::vector<TurnAgg>& byTurn) {
        RunStats st;
        json messages = json::array();
        if (!sc.system.empty()) messages.push_back({ {"role", "system"}, {"content", sc.system} });

        for (size_t i = 0; i < sc.turns.size(); ++i) {
            const int turnNo = static_cast<int>(i) + 1;
            const std::string& userText = sc.turns[i];
            messages.push_back({ {"role", "user"}, {"content", userText} });

            // 요청 본문 = 시나리오 params + 고정 필드 (messages, stream은 덮어씀)
            json body = sc.params;
            body["messages"] = messages;
            body["stream"] = false;

            const auto t0 = std::chrono::steady_clock::now();
            auto res = cli.Post("/v1/chat/completions", Dump(body), "application/json");
            const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

            if (!res) {
                const std::string err = httplib::to_string(res.error());
                std::cerr << "[rep " << rep << " turn " << turnNo << "] connection error: " << err << "\n";
                writeLine({ {"type", "error"}, {"rep", rep}, {"turn", turnNo}, {"kind", "connection"},
                           {"detail", err}, {"wall_ms", wallMs} });
                st.exitCode = kRequestError;
                break;
            }
            if (res->status != 200) {
                std::cerr << "[rep " << rep << " turn " << turnNo << "] HTTP " << res->status << ": " << res->body << "\n";
                writeLine({ {"type", "error"}, {"rep", rep}, {"turn", turnNo}, {"kind", "http"}, {"status", res->status},
                           {"detail", res->body}, {"wall_ms", wallMs} });
                st.exitCode = kRequestError;
                break;
            }

            json resp;
            std::string content;
            std::string reasoning;
            std::string finishReason;
            try {
                resp = json::parse(res->body);
                const auto& choice = resp.at("choices").at(0);
                const auto& message = choice.at("message");
                content = GetStr(message, "content");
                reasoning = GetStr(message, "reasoning_content");
                finishReason = GetStr(choice, "finish_reason");
            }
            catch (const std::exception& e) {
                std::cerr << "[rep " << rep << " turn " << turnNo << "] response parse error: " << e.what() << "\n";
                writeLine({ {"type", "error"}, {"rep", rep}, {"turn", turnNo}, {"kind", "parse"}, {"detail", e.what()},
                           {"body", res->body}, {"wall_ms", wallMs} });
                st.exitCode = kRequestError;
                break;
            }

            const json timings = resp.value("timings", json());   // 없으면 null로 기록
            const json usage = resp.value("usage", json());
            const int promptN = GetNum(timings, "prompt_n", -1);
            const int cacheN = GetNum(timings, "cache_n", -1);
            const int predN = GetNum(timings, "predicted_n", -1);
            const double predMs = GetNum(timings, "predicted_ms", -1.0);
            const double promptMs = GetNum(timings, "prompt_ms", -1.0);
            const double predTps = GetNum(timings, "predicted_per_second", -1.0);

            const checks::ReplyCheck chk = checks::CheckReply(content, sc.checks);
            const bool cut = finishReason == "length";

            json line = {
                {"type", "turn"},
                {"rep", rep},
                {"turn", turnNo},
                {"user", userText},
                {"content", content},
                {"content_empty", content.empty()},
                {"finish_reason", finishReason},
                {"checks", chk.ToJson()},
                {"wall_ms", wallMs},
                {"timings", timings},
                {"usage", usage},
            };
            // reasoning(thinking)이 있으면 길이와 내용 기록. 토큰 수는 서버가 따로 주지 않으므로 글자(바이트) 수만.
            if (!reasoning.empty()) line["reasoning"] = { {"bytes", reasoning.size()}, {"text", reasoning} };
            if (turnNo == 1) line["model"] = resp.value("model", "");
            if (opt.saveRaw) line["raw"] = resp;
            writeLine(line);

            // 다음 턴을 위해 응답을 대화 기록에 추가 (reasoning은 넣지 않음)
            messages.push_back({ {"role", "assistant"}, {"content", content} });

            char buf[288];
            std::snprintf(buf, sizeof(buf),
                "[rep %d turn %d] prompt %d tok (cache %d) %.0f ms | gen %d tok %.0f ms %.1f t/s | wall %.0f ms | %s | %d sent",
                rep, turnNo, promptN, cacheN, promptMs, predN, predMs, predTps, wallMs,
                finishReason.empty() ? "-" : finishReason.c_str(), chk.sentences);
            std::cout << buf << "\n";
            std::string flags = chk.Flags();
            if (cut) flags = std::string("CUT(length)") + (flags.empty() ? "" : " ") + flags;
            if (!flags.empty()) std::cout << "  ! checks: " << flags << "\n";
            if (!reasoning.empty()) {
                std::cout << "  ! reasoning_content present: " << reasoning.size()
                    << " bytes (thinking may be ON - check chat_template_kwargs)\n";
                ++st.reasoningTurns;
            }
            if (content.empty()) {
                std::cout << "  ! WARNING: content is empty (finish_reason " << (finishReason.empty() ? "-" : finishReason)
                    << ")\n";
                ++st.emptyContentTurns;
            }
            if (opt.print) {
                std::cout << "  > " << userText << "\n";
                if (!reasoning.empty()) std::cout << "  ~ " << reasoning << "\n";
                std::cout << "  < " << content << "\n";
            }

            ++st.completed;
            st.wallMs += wallMs;
            if (predN > 0 && predMs > 0) {
                st.predictedTokens += predN;
                st.predictedMs += predMs;
            }
            if (promptN > 0) st.promptTokens += promptN;
            if (cacheN > 0) st.cachedTokens += cacheN;
            if (promptMs > 0) st.promptMs += promptMs;
            st.checks.Add(chk, cut, predN);

            TurnAgg& agg = byTurn[i];
            ++agg.n;
            if (predN > 0) agg.predicted += predN;
            agg.checks.Add(chk, cut, predN);
        }
        return st;
    }

    json SummaryJson(const RunStats& st, size_t totalTurns) {
        return {
            {"completed_turns", st.completed},
            {"total_turns", totalTurns},
            {"total_predicted_tokens", st.predictedTokens},
            {"total_predicted_ms", st.predictedMs},
            {"total_prompt_tokens", st.promptTokens},
            {"total_cached_tokens", st.cachedTokens},
            {"total_prompt_ms", st.promptMs},
            {"total_wall_ms", st.wallMs},
            {"gen_tps_server", st.GenTpsServer()},   // 생성 토큰 합 / 서버 predicted_ms 합 (순수 생성 속도)
            {"gen_tps_wall", st.GenTpsWall()},       // 생성 토큰 합 / 하네스 왕복 시간 합 (입력 처리·HTTP 포함)
            {"empty_content_turns", st.emptyContentTurns},
            {"reasoning_turns", st.reasoningTurns},
            {"checks", st.checks.ToJson(st.completed)},
            {"exit_code", st.exitCode},
        };
    }

    void PrintChecksLine(const CheckCounts& c, int n) {
        char buf[448];
        std::snprintf(buf, sizeof(buf),
            "       checks: taboo %d/%d, forbidden %d/%d, stage %d/%d, emoji %d/%d, latin %d/%d, name %d/%d, "
            "quoted %d/%d, long %d/%d, short %d/%d, cut(length) %d/%d | names %d/%d | avg %.1f sent, max gen %d tok",
            c.taboo, n, c.forbidden, n, c.stage, n, c.emoji, n, c.latin, n, c.namePrefix, n, c.quoted, n,
            c.sentOver, n, c.sentUnder, n, c.lengthCut, n, c.residents, n,
            n > 0 ? static_cast<double>(c.sentences) / n : 0.0,
            c.maxPredicted);
        std::cout << buf << "\n";
    }

    void PrintSummaryLine(const char* head, const RunStats& st, size_t totalTurns) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "%s %d/%zu turns | gen %lld tok, server %.1f t/s, wall-based %.1f t/s | "
            "prompt %lld new + %lld cached tok, %.0f ms | wall %.2f s",
            head, st.completed, totalTurns, st.predictedTokens, st.GenTpsServer(), st.GenTpsWall(),
            st.promptTokens, st.cachedTokens, st.promptMs, st.wallMs / 1000.0);
        std::cout << buf << "\n";
        PrintChecksLine(st.checks, st.completed);
        if (st.emptyContentTurns > 0 || st.reasoningTurns > 0)
            std::cout << "       ! empty content " << st.emptyContentTurns << " turns, reasoning " << st.reasoningTurns
            << " turns\n";
    }

}  // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   // 콘솔 한글 출력
#endif

    auto parsed = ParseArgs(argc, argv);
    if (!parsed) {
        PrintUsage();
        return kUsage;
    }
    const Options opt = *parsed;

    httplib::Client cli(opt.host, opt.port);
    cli.set_connection_timeout(std::chrono::seconds(2));
    cli.set_read_timeout(std::chrono::seconds(300));    // 첫 턴 워밍업이 수십 초 걸릴 수 있음
    cli.set_write_timeout(std::chrono::seconds(10));

    if (opt.healthOnly) return WaitForServer(cli, opt.waitSeconds);

    // 시나리오(렌더링 포함)를 서버보다 먼저 확인: 데이터 오류는 서버 없이도 바로 보이게
    auto scenario = LoadScenario(opt);
    if (!scenario) return kScenarioError;

    if (opt.render) return RunRender(cli, opt, *scenario);

    if (int rc = WaitForServer(cli, opt.waitSeconds); rc != kOk) return rc;
    const ModelInfo model = FetchModelInfo(cli);

    if (opt.tokenize) return RunTokenize(cli, opt, *scenario, model);

    // 결과 파일 열기
    std::error_code ec;
    fs::create_directories(fs::path(opt.outDir), ec);
    const fs::path outPath = MakeOutPath(opt, *scenario, model, "", ".jsonl");
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "[out] cannot create: " << outPath.string() << "\n";
        return kScenarioError;
    }
    auto writeLine = [&](const json& j) {
        out << Dump(j) << "\n";
        out.flush();   // 중간에 죽어도 그때까지의 결과는 남게
        };

    writeLine({
        {"type", "run"},
        {"harness", kHarnessVersion},
        {"scenario", scenario->name},
        {"scenario_file", opt.scenarioPath},
        {"label", opt.label},
        {"model", model.id},
        {"models_raw", model.raw},
        {"started_at", NowStamp("%Y-%m-%dT%H:%M:%S")},
        {"server", opt.host + ":" + std::to_string(opt.port)},
        {"prompt_source", PromptSourceJson(*scenario)},
        {"checks_config", CheckConfigJson(scenario->checks)},
        {"system", scenario->system},
        {"params", scenario->params},
        {"turn_count", scenario->turns.size()},
        {"repeat", opt.repeat},
        });

    std::cout << "[run] " << scenario->name;
    if (scenario->npc) std::cout << " (" << scenario->npc->templateTag << ", level " << scenario->npc->level << ")";
    std::cout << ", " << scenario->turns.size() << " turns x " << opt.repeat << ", max_tokens "
        << (scenario->params.contains("max_tokens") ? Dump(scenario->params["max_tokens"]) : "-") << " -> "
        << outPath.string() << "\n";

    const size_t nTurns = scenario->turns.size();
    std::vector<RunStats> reps;
    std::vector<TurnAgg> byTurn(nTurns);
    int exitCode = kOk;

    for (int rep = 1; rep <= opt.repeat; ++rep) {
        // (v0.3) 회차마다 seed를 바꿔 다시 렌더링 (|pick이 없으면 결과는 같음)
        if (scenario->npc) {
            std::string err;
            if (!prompt::RenderNpcPrompt(*scenario->npc, opt.seed + rep - 1, err)) {
                std::cerr << "[render] " << err << "\n";
                exitCode = kScenarioError;
                break;
            }
            scenario->system = scenario->npc->system;
        }
        RunStats st = RunOnce(cli, opt, *scenario, rep, writeLine, byTurn);
        json s = SummaryJson(st, nTurns);
        s["type"] = "summary";
        s["rep"] = rep;
        if (scenario->npc) {
            s["seed"] = scenario->npc->seed;
            s["picks"] = scenario->npc->picks;
            if (!scenario->npc->picks.empty()) s["system"] = scenario->system;
        }
        writeLine(s);

        char head[32];
        std::snprintf(head, sizeof(head), "[rep %d done]", rep);
        PrintSummaryLine(head, st, nTurns);

        reps.push_back(st);
        if (st.exitCode != kOk) {
            exitCode = st.exitCode;
            break;   // 실패하면 남은 회차는 돌리지 않음
        }
    }

    if (opt.repeat > 1) {
        RunStats all;
        json perRep = json::array();
        for (size_t r = 0; r < reps.size(); ++r) {
            const auto& st = reps[r];
            all.completed += st.completed;
            all.predictedTokens += st.predictedTokens;
            all.predictedMs += st.predictedMs;
            all.promptTokens += st.promptTokens;
            all.cachedTokens += st.cachedTokens;
            all.promptMs += st.promptMs;
            all.wallMs += st.wallMs;
            all.emptyContentTurns += st.emptyContentTurns;
            all.reasoningTurns += st.reasoningTurns;
            all.checks.Merge(st.checks);
            perRep.push_back({ {"rep", r + 1}, {"gen_tps_server", st.GenTpsServer()},
                              {"gen_tps_wall", st.GenTpsWall()}, {"total_wall_ms", st.wallMs},
                              {"taboo_turns", st.checks.taboo} });
        }
        all.exitCode = exitCode;

        json byTurnJson = json::array();
        for (size_t i = 0; i < nTurns; ++i) {
            json t = byTurn[i].checks.ToJson(byTurn[i].n);
            t["turn"] = i + 1;
            t["user"] = scenario->turns[i];
            t["avg_predicted_tokens"] = byTurn[i].n > 0 ? static_cast<double>(byTurn[i].predicted) / byTurn[i].n : 0.0;
            byTurnJson.push_back(t);
        }

        json s = SummaryJson(all, nTurns * reps.size());
        s["type"] = "summary_all";
        s["reps_run"] = reps.size();
        s["per_rep"] = perRep;
        s["by_turn"] = byTurnJson;
        writeLine(s);
        PrintSummaryLine("[all done]", all, nTurns * reps.size());

        std::cout << "[by turn]\n";
        for (size_t i = 0; i < nTurns; ++i) {
            const TurnAgg& a = byTurn[i];
            const CheckCounts& c = a.checks;
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "  t%zu: taboo %d/%d, stage %d/%d, forbidden %d/%d, long %d/%d, cut %d/%d, names %d/%d | avg %.0f tok, max %d, avg %.1f sent",
                i + 1, c.taboo, a.n, c.stage, a.n, c.forbidden, a.n, c.sentOver, a.n, c.lengthCut, a.n, c.residents, a.n,
                a.n > 0 ? static_cast<double>(a.predicted) / a.n : 0.0, c.maxPredicted,
                a.n > 0 ? static_cast<double>(c.sentences) / a.n : 0.0);
            std::cout << buf << "\n";
        }
    }
    return exitCode;
}