// reply_checks.h — NPC 응답 자동 점검 (llm-harness v0.3)
//
// 문자열 규칙만으로 하는 1차 점검이다. 높임법 일관성·자연스러움은 사용자가 직접 본다.
// 문장 수는 마침표·물음표·느낌표·줄바꿈 기준 대략값 (말줄임 "..."/"…"는 문장 끝으로 안 셈).
#pragma once

#include "prompt_render.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace checks {

    struct ReplyCheck {
        std::vector<std::string> tabooHits;       // 금기 키워드
        std::vector<std::string> forbiddenHits;   // 금지어 (세계 공통 + NPC별)
        std::vector<std::string> residentHits;    // (v0.3) 응답에 나온 다른 마을 사람 이름 (실패가 아니라 기록용)
        int stageMarks = 0;      // 지문 의심 기호: ( （ * [
        int emoji = 0;           // 이모지·그림 기호
        int latin = 0;           // 영문 알파벳 글자 수
        bool namePrefix = false; // "마르타:"처럼 이름표로 시작
        bool quoted = false;     // 응답 전체가 따옴표로 시작 (예시 대사 형식 따라 하기)
        int sentences = 0;       // 대략 문장 수
        int sentenceRangeMin = 0, sentenceRangeMax = 0;  // 레벨 기준 (0이면 점검 안 함)

        bool SentencesOver() const { return sentenceRangeMax > 0 && sentences > sentenceRangeMax; }
        bool SentencesUnder() const { return sentenceRangeMin > 0 && sentences < sentenceRangeMin; }
        nlohmann::json ToJson() const;
        std::string Flags() const;   // 콘솔용 짧은 표시. 문제가 없으면 빈 문자열
    };

    ReplyCheck CheckReply(const std::string& text, const prompt::CheckConfig& cfg);

    int CountSentences(const std::string& text);

}  // namespace checks