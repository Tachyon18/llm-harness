// prompt_render.h — NPC 시스템 프롬프트 렌더링 (llm-harness v0.3)
//
// 세계 설정(world/*.json) + NPC 데이터(npcs/*.json) + 템플릿(prompts/*.txt)을 합쳐
// 시스템 프롬프트 문자열을 만든다. 단계 2(UE)로 옮길 것을 생각해 HTTP·콘솔과 분리해 둔다.
//
// 템플릿 문법 (의도적으로 최소한만):
//   {{world.name}}, {{npc.taboo.topic}}, {{level.rule}}   점(.)으로 구분한 경로
//   - 문자열        → 그대로
//   - 숫자·참/거짓  → 문자열로
//   - 문자열 배열   → "- 항목" 줄들 (빈 배열은 "- (없음)"). 배열 자리는 줄 단독으로 둘 것
//   - 객체·null·없는 키 → 오류 (조용히 빈칸이 되는 것을 막음)
//   {{npc.taboo.deflect_examples|pick}}   (v0.3) 문자열 배열에서 하나만 골라 넣는다.
//       고르는 규칙: (seed - 1) % 개수 → 하네스는 회차(rep)마다 seed를 1씩 올려 예시를 돌려 쓴다.
//       게임에서는 엔진이 대화(세션)마다 seed를 정하는 자리.
// 렌더 컨텍스트는 { "world": 세계, "npc": NPC, "level": 세계.dialogue_levels["<npc.level>"] }.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace prompt {

    using json = nlohmann::json;

    // 자동 점검에 쓰는 값 (모델에는 보내지 않음)
    struct CheckConfig {
        std::string npcName;                       // "마르타:" 같은 이름표 감지용
        std::vector<std::string> tabooKeywords;    // npc.eval.taboo_keywords
        std::vector<std::string> forbiddenWords;   // world.forbidden_words + npc.eval.forbidden_words
        std::vector<std::string> residentNames;    // (v0.3) world.residents의 ':' 앞 이름, 자기 자신 제외
        int minSentences = 0;                      // 0이면 점검 안 함
        int maxSentences = 0;
    };

    struct NpcPrompt {
        std::string system;            // 렌더링 결과
        std::string worldPath, npcPath, templatePath;
        std::string templateTag;       // 템플릿 파일 이름(확장자 제외), 결과 파일 이름용
        int level = 0;
        int levelMaxTokens = 0;        // 레벨 하드 캡 (0이면 없음)
        std::vector<std::string> usedKeys;   // 템플릿이 실제로 쓴 키 (기록용)
        CheckConfig checks;
        // (v0.3) seed를 바꿔 다시 렌더링하기 위한 원본
        std::string templateText;
        json ctx;
        int seed = 1;
        json picks = json::array();    // 마지막 렌더링에서 |pick이 고른 것 [{key, index, count}]
    };

    // 파일을 UTF-8 텍스트로 읽는다. BOM 제거, CRLF/CR → LF. 실패 시 nullopt, err에 이유
    std::optional<std::string> ReadTextFile(const std::string& path, std::string& err);
    std::optional<json> ReadJsonFile(const std::string& path, std::string& err);

    // 템플릿 치환. 실패하면 false, err에 줄 번호와 키
    bool RenderTemplate(const std::string& tpl, const json& ctx, std::string& out, std::string& err,
        std::vector<std::string>* usedKeys = nullptr, int seed = 1, json* picks = nullptr);

    // (v0.3) 같은 데이터로 seed만 바꿔 다시 렌더링 (p.system, p.seed, p.picks 갱신)
    bool RenderNpcPrompt(NpcPrompt& p, int seed, std::string& err);

    // 세 파일을 읽어 렌더링하고 레벨·점검 설정까지 채운다 (seed 기본 1)
    std::optional<NpcPrompt> BuildNpcPrompt(const std::string& worldPath, const std::string& npcPath,
        const std::string& templatePath, std::string& err, int seed = 1);

}  // namespace prompt