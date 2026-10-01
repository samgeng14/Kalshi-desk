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
  src/universe.cpp
  src/wxdist.cpp
  src/probability.cpp
  src/edge.cpp
  src/decision_router.cpp
  src/decision_log.cpp
  src/systematic.cpp
  src/lanes.cpp
  src/perps.cpp
  src/poly.cpp
  src/pair_models.cpp
  src/account_summary.cpp
  src/record.cpp
  src/hedge.cpp
  src/ladder.cpp
  src/engine.cpp
  src/executor.cpp
  src/manual_orders.cpp
  src/eval.cpp
  src/server.cpp
  src/hypotheses.cpp
  src/research.cpp
  src/ws_client.cpp
  src/reference_feed.cpp
  src/models/terminal_price.cpp
  src/strategies/registry.cpp
  src/strategies/manual_fair.cpp
  src/strategies/xvenue.cpp
  src/strategies/crypto_lognormal.cpp
  src/strategies/commodity_terminal.cpp
  src/strategies/weather_nws.cpp
  src/strategies/sports_inplay.cpp
  src/strategies/sports_live.cpp
  src/strategies/sportsbook_line.cpp
  src/strategies/sports_totals.cpp
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
target_compile_definitions(desk-tests PRIVATE DESK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME unit COMMAND desk-tests)

add_executable(systematic-tests tests/test_systematic.cpp)
target_link_libraries(systematic-tests PRIVATE desk)
add_test(NAME systematic COMMAND systematic-tests)

add_executable(perps-tests tests/test_perps.cpp)
target_link_libraries(perps-tests PRIVATE desk)
add_test(NAME perps COMMAND perps-tests)

add_executable(poly-tests tests/test_poly.cpp)
target_link_libraries(poly-tests PRIVATE desk)
add_test(NAME poly COMMAND poly-tests)

add_executable(account-summary-tests tests/test_account_summary.cpp)
target_link_libraries(account-summary-tests PRIVATE desk)
add_test(NAME account-summary COMMAND account-summary-tests)

add_executable(pipeline-tests tests/test_pipeline.cpp)
target_link_libraries(pipeline-tests PRIVATE desk)
add_test(NAME pipeline COMMAND pipeline-tests)

add_executable(commodity-tests tests/test_commodity.cpp)
target_link_libraries(commodity-tests PRIVATE desk)
target_compile_options(commodity-tests PRIVATE -Wall -Wextra)
add_test(NAME commodity COMMAND commodity-tests)

add_executable(routing-tests tests/test_routing.cpp)
target_link_libraries(routing-tests PRIVATE desk)
add_test(NAME routing COMMAND routing-tests)

add_executable(manual-order-tests tests/test_manual_orders.cpp)
target_link_libraries(manual-order-tests PRIVATE desk)
add_test(NAME manual-orders COMMAND manual-order-tests)

add_executable(pair-model-tests tests/test_pair_models.cpp)
target_link_libraries(pair-model-tests PRIVATE desk)
add_test(NAME pair-models COMMAND pair-model-tests)

# The dashboard's category routes, checked straight out of ui/index.html. No browser and no
# node_modules: the Playwright suites need both, this one needs neither, so it runs everywhere.
find_program(NODE_EXE NAMES node)
if(NODE_EXE)
  add_test(NAME ui-routes COMMAND ${NODE_EXE} ${CMAKE_SOURCE_DIR}/tests/test_ui_routes.cjs)
  # The operator-facing structures: lanes, the self-explaining empty board, the funnel, halted
  # never reading as running, and the ticket's side logic. Also reads the page, no browser.
  add_test(NAME ui-desk-ux COMMAND ${NODE_EXE} ${CMAKE_SOURCE_DIR}/tests/test_ui_desk_ux.cjs)
endif()

# Recorder data integrity: unit conversion and sequence-gap handling, no network required.
find_program(PYTHON3_EXE NAMES python3)
if(PYTHON3_EXE)
  add_test(NAME recorder COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tests/test_recorder.py)
  # The alpha audit checked against ground truth: pure noise must yield nothing, an injected
  # signal must be found, and the HAC correction must shrink the t-stat overlap inflates.
  add_test(NAME alpha-decay COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tests/test_alpha_decay.py)
  # The commodity replay's own proof that it cannot see the future: it builds its data in memory,
  # moves the reference after a chosen instant and checks no earlier probability changed.
  add_test(NAME commodity-replay
           COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/research/commodity_terminal.py --self-test)
  # The research ladder's own checks: an overconfident model must fail the calibration slope, an
  # inverted edge must fail the monotonicity requirement, and no stage may issue a verdict below
  # the registered sample.
  add_test(NAME commodity-ladder
           COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/research/commodity_ladder.py --self-test)
  # The channel-behaviour report refuses to conclude from a short recording, and a database that
  # predates the observability fields is described rather than misread.
  add_test(NAME commodity-channel
           COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/research/commodity_channel_behavior.py --self-test)
  # The impulse lane's causal audit: an adversarial target built on pre-entry information must be
  # refused, and provenance must survive arithmetic so it cannot be laundered.
  add_test(NAME impulse-audit
           COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/research/impulse_target_audit.py --self-test)
  add_test(NAME impulse-prepare
           COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/research/impulse_prepare.py --self-test)
  # The settlement-gate census still counts every Polymarket pair the xvenue strategy saw fresh,
  # although poly_markets.quote_ts is written at most every 60 s since 2026-10-01.
  add_test(NAME settlement-census
           COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/research/settlement_gate_census.py --self-test)
endif()
