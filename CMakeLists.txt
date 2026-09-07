cmake_minimum_required(VERSION 3.16)
project(kalshi_desk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- dependencies -----------------------------------------------------------
# macOS: OpenSSL comes from Homebrew (brew install openssl@3). curl and sqlite3
# ship with the Xcode command line tools.
if(APPLE)
  execute_process(COMMAND brew --prefix openssl@3
                  OUTPUT_VARIABLE BREW_OPENSSL
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(BREW_OPENSSL AND NOT OPENSSL_ROOT_DIR)
    set(OPENSSL_ROOT_DIR "${BREW_OPENSSL}")
  endif()
endif()
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
# CMake's FindSQLite3 exports SQLite::SQLite3; some newer/Homebrew configs export SQLite3::SQLite3.
if(TARGET SQLite::SQLite3)
  set(DESK_SQLITE_TARGET SQLite::SQLite3)
elseif(TARGET SQLite3::SQLite3)
  set(DESK_SQLITE_TARGET SQLite3::SQLite3)
else()
  message(FATAL_ERROR "SQLite3 found but no imported target")
endif()
find_package(Threads REQUIRED)

# ---- embed the dashboard HTML into the binary --------------------------------
set(UI_SRC ${CMAKE_SOURCE_DIR}/ui/index.html)
set(UI_HDR ${CMAKE_BINARY_DIR}/generated/ui_html.h)
add_custom_command(
  OUTPUT ${UI_HDR}
  COMMAND ${CMAKE_COMMAND} -DIN=${UI_SRC} -DOUT=${UI_HDR}
          -P ${CMAKE_SOURCE_DIR}/cmake/embed_ui.cmake
  DEPENDS ${UI_SRC} ${CMAKE_SOURCE_DIR}/cmake/embed_ui.cmake
  COMMENT "Embedding ui/index.html")
add_custom_target(ui_embed DEPENDS ${UI_HDR})

# ---- library ----------------------------------------------------------------
add_library(desk STATIC
  src/types.cpp
  src/config.cpp
  src/auth.cpp
  src/http.cpp
  src/client.cpp
  src/store.cpp
  src/fees.cpp
  src/risk.cpp
  src/hedge.cpp
  src/ladder.cpp
  src/engine.cpp
  src/executor.cpp
  src/eval.cpp
  src/server.cpp
  src/hypotheses.cpp
  src/research.cpp
  src/strategies/registry.cpp
  src/strategies/manual_fair.cpp
  src/strategies/crypto_lognormal.cpp
  src/strategies/weather_nws.cpp
  src/strategies/sports_inplay.cpp
  src/strategies/sports_live.cpp
  src/strategies/stubs.cpp)
add_dependencies(desk ui_embed)
target_include_directories(desk PUBLIC include third_party ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(desk PUBLIC
  OpenSSL::SSL OpenSSL::Crypto CURL::libcurl ${DESK_SQLITE_TARGET} Threads::Threads)
target_compile_options(desk PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- executables ------------------------------------------------------------
add_executable(kalshi-desk src/main.cpp)
target_link_libraries(kalshi-desk PRIVATE desk)
target_compile_options(kalshi-desk PRIVATE -Wall -Wextra)

add_executable(desk-tests tests/test_main.cpp)
target_link_libraries(desk-tests PRIVATE desk)

enable_testing()
add_test(NAME unit COMMAND desk-tests)
