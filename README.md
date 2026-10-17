# llm-harness

로컬 LLM(llama.cpp)을 게임 NPC·추리 진행자로 쓰기 위한 테스트 하네스 (C++).
시나리오(JSON)를 llama-server에 여러 턴으로 보내고, 응답·토큰 수·시간·자동 점검 결과를 JSONL로 기록한다.

## 현재 버전
- 하네스 v0.3: NPC 프롬프트 렌더링(세계 + NPC 데이터 + 템플릿), `|pick`(대화마다 예시 대사 돌려 쓰기), 레벨별 응답 길이 상한, 자동 점검(금기 키워드·금지어·지문·이모지·문장 수 등)
- NPC 프롬프트 v1: `prompts/npc_v1_C3.txt` + `world/mistvale.json` + `npcs/*.json` (마르타·레온·에블린)

## 준비물
- Windows, Visual Studio 2022 (MSVC v143), CMake 3.24 이상
- llama.cpp b11430 Windows Vulkan판 (`llama-server.exe`)
- 모델: `google/gemma-4-E4B-it-qat-q4_0-gguf` (저장소에 포함하지 않음, 서버가 `-hf`로 받음)

## 빌드
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

## 실행
서버:
    llama-server.exe -hf google/gemma-4-E4B-it-qat-q4_0-gguf -ngl all -c 8192 --port 8081 --no-mmproj-auto
하네스:
    build\Release\llm-harness.exe scenarios\marta_taboo.json --repeat 10 --label test
    build\Release\llm-harness.exe scenarios\npc_common.json --npc npcs\leon.json --render

## 폴더
- `src/` 하네스 소스 (main, prompt_render, reply_checks)
- `world/`, `npcs/`, `prompts/` NPC 프롬프트 데이터·템플릿
- `scenarios/` 테스트 시나리오
- `results/samples/` 대표 실행 결과 (전체 결과는 커밋하지 않음)

## 버전 기록
| 버전 | 내용 |
| --- | --- |
| v0 | 시나리오 → 서버 호출 → JSONL 기록 |
| v0.1 | reasoning 기록, --repeat, --label, --tokenize, 모델 태그 |
| v0.2 | 세계·NPC·템플릿 렌더링, --render, --template, 레벨별 max_tokens, 자동 점검 |
| v0.3 | `|pick`, --npc, --seed, 마을 사람 이름 점검 |

## 의존성
nlohmann/json 3.12.0, cpp-httplib 0.53.1 (둘 다 MIT, CMake FetchContent로 받음)