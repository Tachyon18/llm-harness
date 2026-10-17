# =============================================================================
# llm-harness — LLM-GM 단계 1 테스트 하네스 (llama-server HTTP 클라이언트)
#
# 의존성 (처음 구성(configure)할 때 CMake가 GitHub에서 자동으로 받음, git 불필요):
#   - nlohmann/json v3.12.0 (JSON, 헤더 전용, MIT)
#   - cpp-httplib   v0.53.1 (HTTP 클라이언트, 헤더 전용, MIT)
# 버전은 고정한다. 올릴 때는 아래 URL의 태그만 바꾸고 build 폴더를 지운 뒤 다시 구성한다.
# =============================================================================
cmake_minimum_required(VERSION 3.24)
project(llm_harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# --- nlohmann/json -----------------------------------------------------------
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# --- cpp-httplib -------------------------------------------------------------
# 로컬 http://127.0.0.1 만 쓰므로 HTTPS(OpenSSL)와 압축 라이브러리는 모두 끈다.
# (기본값 ON이면 PC에 해당 라이브러리가 깔려 있는지에 따라 빌드 결과가 달라짐)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.53.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(json httplib)

# --- 실행 파일 ---------------------------------------------------------------
# .cpp 파일을 새로 만들면 여기에 한 줄씩 추가한다.
add_executable(llm-harness
    src/main.cpp
    src/prompt_render.cpp     # v0.2: world + npc + template → 시스템 프롬프트
    src/reply_checks.cpp      # v0.2: 응답 자동 점검
)

target_link_libraries(llm-harness PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib          # Windows에서는 ws2_32, crypt32가 자동으로 함께 링크됨
)

if(MSVC)
    target_compile_options(llm-harness PRIVATE
        /utf-8                # 소스·실행 문자 집합을 UTF-8로 (한글 문자열 깨짐 방지)
        /W4                   # 경고 수준
        /permissive-          # 표준 준수 모드
    )
    target_compile_definitions(llm-harness PRIVATE
        NOMINMAX              # windows.h의 min/max 매크로가 std::min/std::max를 깨는 것 방지
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00   # Windows 10 이상 (cpp-httplib 요구 사항)
    )
    # Visual Studio에서 F5로 실행할 때 작업 폴더 = 프로젝트 루트 (scenarios/, results/ 상대 경로용)
    set_property(TARGET llm-harness PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # .sln을 열었을 때 시작 프로젝트를 llm-harness로
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT llm-harness)
else()
    target_compile_options(llm-harness PRIVATE -Wall -Wextra)
endif()
