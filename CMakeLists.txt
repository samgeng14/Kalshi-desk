cmake_minimum_required(VERSION 3.16)
project(kalshi_desk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- build speed and build safety (2026-10-06) --------------------------------
# Configure with -G Ninja. Ninja runs ncpu + 2 jobs (12 on this Mac) when not told otherwise,
# and an unbounded build once stalled the live desk for 18 minutes (2026-09-24). Under Ninja
# every compile, PCH included, goes through a pool of DESK_BUILD_JOBS and every link through 2,
# whatever -j says. Makefiles ignore pools: there, -j 4 is still on you.
set(DESK_BUILD_JOBS 4 CACHE STRING "most compile jobs at once under Ninja (the live desk's limit)")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS desk_compile=${DESK_BUILD_JOBS} desk_link=2)
set(CMAKE_JOB_POOL_COMPILE desk_compile)
set(CMAKE_JOB_POOL_PRECOMPILE_HEADER desk_compile)
set(CMAKE_JOB_POOL_LINK desk_link)

# The compiler runs native on Apple Silicon, whatever drives it. /usr/local/bin/ninja and the
# Intel cmake are x86_64 builds, and a universal clang started under them runs as x86_64 too,
# under Rosetta 2 (/usr/bin/arch printed i386 through that chain). Measured 2026-10-06 at nice 15:
# server.cpp 10.15 s under Rosetta against 3.64 s native (3.90 s with an x86 parent and
# arch -arm64), lock.cpp 0.92 s against 0.08 s. Every compile and link therefore starts through
# "arch -arm64" on an arm64 host; Intel Macs and Linux are untouched.
set(DESK_NATIVE_PREFIX "")
if(APPLE)
  execute_process(COMMAND /usr/sbin/sysctl -n hw.optional.arm64
                  OUTPUT_VARIABLE DESK_HOST_ARM64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(DESK_HOST_ARM64 STREQUAL "1")
    set(DESK_NATIVE_PREFIX /usr/bin/arch -arm64)
  endif()
endif()

# ccache when it is installed (brew install ccache); the build works the same without it.
# deskd precompiles json.hpp and httplib.h, and ccache misses on every file that uses a PCH
# unless it ignores the PCH's defines and the header times (the PCH is rebuilt with a new mtime
# and ctime) and clang leaves its build time out of the PCH (-fno-pch-timestamp, on deskd's
# targets below). The sloppiness rides on the launcher, so no ~/.config/ccache edit is needed:
# ccache 4.8 and later read KEY=VALUE settings before the compiler; older ones get them through
# "cmake -E env" (one more process per compile, 0.84 s of it under Rosetta).
find_program(CCACHE_EXE ccache)
set(DESK_CCACHE_SLOPPINESS "pch_defines,time_macros,include_file_mtime,include_file_ctime")
if(CCACHE_EXE AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  execute_process(COMMAND ${CCACHE_EXE} --version OUTPUT_VARIABLE DESK_CCACHE_VERSION ERROR_QUIET)
  string(REGEX MATCH "version ([0-9]+\\.[0-9]+)" _ "${DESK_CCACHE_VERSION}")
  if(CMAKE_MATCH_1 AND CMAKE_MATCH_1 VERSION_GREATER_EQUAL 4.8)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${DESK_NATIVE_PREFIX} ${CCACHE_EXE} "sloppiness=${DESK_CCACHE_SLOPPINESS}")
  else()
    set(CMAKE_CXX_COMPILER_LAUNCHER
        ${DESK_NATIVE_PREFIX} ${CMAKE_COMMAND} -E env "CCACHE_SLOPPINESS=${DESK_CCACHE_SLOPPINESS}" ${CCACHE_EXE})
  endif()
  message(STATUS "ccache: ${CCACHE_EXE} (${CMAKE_MATCH_1})")
elseif(NOT CCACHE_EXE)
  message(STATUS "ccache: not found, building without it (brew install ccache)")
  if(DESK_NATIVE_PREFIX AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${DESK_NATIVE_PREFIX})
  endif()
endif()
if(DESK_NATIVE_PREFIX AND NOT DEFINED CMAKE_CXX_LINKER_LAUNCHER)
  set(CMAKE_CXX_LINKER_LAUNCHER ${DESK_NATIVE_PREFIX})   # CMake 3.21 and later; ignored before
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
# CMake's FindSQLite3 exports SQLite::SQLite3; some newer/Homebrew configs export SQLite3::SQLite3,
# and CMake 4 marks the old name deprecated (a warning per target), so the new name goes first.
if(TARGET SQLite3::SQLite3)
  set(DESK_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  set(DESK_SQLITE_TARGET SQLite::SQLite3)
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
# markets_write_mode: skip mode must read exactly what write mode reads (see the test's header).
add_executable(invariance-tests tests/test_invariance.cpp)
target_link_libraries(invariance-tests PRIVATE desk)
add_test(NAME markets-invariance COMMAND invariance-tests)

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
  # The v3 screen's foundation (ui/desk): formatting, the store, sequence-gap resync, the demo
  # venues' matching rules and fees, and every simulated event checked against the backend
  # contract (ui/desk/js/contract.js). Loads the scripts into a node vm; no browser, no packages.
  add_test(NAME ui-desk-v3 COMMAND ${NODE_EXE} ${CMAKE_SOURCE_DIR}/tests/test_ui_desk_v3.cjs)
  # The v3 MANUAL and SYSTEMATIC tabs: the real index.html booted into a small fake DOM on the
  # demo venues with a fake clock (clicks, keys, order states, KILL and re-arm, WIDE mode).
  add_test(NAME ui-desk-v3-manual COMMAND ${NODE_EXE} ${CMAKE_SOURCE_DIR}/tests/test_ui_desk_v3_manual.cjs)
  add_test(NAME ui-desk-v3-systematic COMMAND ${NODE_EXE} ${CMAKE_SOURCE_DIR}/tests/test_ui_desk_v3_systematic.cjs)
endif()

# Recorder data integrity: unit conversion and sequence-gap handling, no network required.
find_program(PYTHON3_EXE NAMES python3)
if(PYTHON3_EXE)
  add_test(NAME recorder COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tests/test_recorder.py)
  # The alpha audit checked against ground truth: pure noise must yield nothing, an injected
  # signal must be found, and the HAC correction must shrink the t-stat overlap inflates.
  add_test(NAME alpha-decay COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tests/test_alpha_decay.py)
  add_test(NAME latency-report COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/latency_report.py --self-test)
  add_test(NAME train-wcbb COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/train_wcbb.py --self-test)
  add_test(NAME replication COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/replication.py --self-test)
  add_test(NAME lip-join COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/lip_join.py --self-test)
  add_test(NAME fillsim2 COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/fillsim2.py --self-test)
  add_test(NAME scorecard COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/tools/scorecard.py --self-test)
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

# ---- deskd: the event-driven desk (plan section 2) ----------------------------
# New code only: src/deskd, include/deskd, tests/deskd. It shares nothing with the desk library
# above, which is deleted on 2026-11-22. Small headers (one per owner, a pimpl for the web
# server) keep a strategy edit to one compile and a link; json.hpp and httplib.h are
# precompiled once and reused by every deskd target, so no deskd file parses them again.
#
# CMake asks clang to instantiate the PCH's templates once, inside the PCH, but not Apple clang
# (Modules/Compiler/Clang.cmake: "AND NOT __is_apple_clang"). Without it every file pays for
# json.hpp's and httplib.h's pending instantiations: measured 2026-10-06 under nice 15,
# lock.cpp took 1.28 s with the PCH against 0.32 s without it (890 ms in
# PerformPendingInstantiations), and 0.11 s once the PCH instantiates them.
include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND NOT CMAKE_CXX_COMPILE_OPTIONS_INSTANTIATE_TEMPLATES_PCH)
  check_cxx_compiler_flag(-fpch-instantiate-templates DESKD_HAS_PCH_INSTANTIATE)
  if(DESKD_HAS_PCH_INSTANTIATE)
    set(CMAKE_CXX_COMPILE_OPTIONS_INSTANTIATE_TEMPLATES_PCH -fpch-instantiate-templates)
  endif()
endif()

# The plan's soak runs under ThreadSanitizer (plan section 8): configure a separate build with
# -DDESKD_TSAN=ON; every target, the fake exchange included, is instrumented.
option(DESKD_TSAN "build deskd and its tests under ThreadSanitizer" OFF)
if(DESKD_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()
# ... and under AddressSanitizer with UndefinedBehaviorSanitizer (plan section 8: both sanitizers):
# -DDESKD_ASAN=ON, never together with DESKD_TSAN. Any UB report fails the test that hit it.
option(DESKD_ASAN "build deskd and its tests under AddressSanitizer and UBSan" OFF)
if(DESKD_ASAN AND DESKD_TSAN)
  message(FATAL_ERROR "DESKD_ASAN and DESKD_TSAN cannot be combined: configure one per build")
endif()
if(DESKD_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()
add_library(deskd_flags INTERFACE)
target_include_directories(deskd_flags INTERFACE include ${CMAKE_BINARY_DIR}/generated)
target_include_directories(deskd_flags SYSTEM INTERFACE third_party)
target_compile_options(deskd_flags INTERFACE -Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # no build time inside the PCH: with it, every PCH rebuild changes the hash ccache keys on
  target_compile_options(deskd_flags INTERFACE -Xclang -fno-pch-timestamp)
endif()
target_link_libraries(deskd_flags INTERFACE Threads::Threads)

add_library(deskd_core STATIC
  src/deskd/bus/units.cpp
  src/deskd/bus/msg.cpp
  src/deskd/app/log.cpp
  src/deskd/app/worker.cpp
  src/deskd/app/lock.cpp
  src/deskd/app/config.cpp
  src/deskd/app/ops.cpp
  src/deskd/app/runtime.cpp
  src/deskd/app/venue.cpp
  src/deskd/app/projection.cpp
  src/deskd/md/feed.cpp
  src/deskd/md/subscribe.cpp
  src/deskd/md/kalshi_md.cpp
  src/deskd/rest/state.cpp
  src/deskd/priv/kalshi_private.cpp
  src/deskd/ref/pyth.cpp
  src/deskd/ref/espn.cpp
  src/deskd/net/guard.cpp
  src/deskd/net/http.cpp
  src/deskd/net/ws.cpp
  src/deskd/net/conn.cpp
  src/deskd/net/pool.cpp
  src/deskd/net/send.cpp
  src/deskd/net/kalshi_sender.cpp
  src/deskd/gw/gateway.cpp
  src/deskd/gw/order_ledger.cpp
  src/deskd/gw/manual_admission.cpp
  src/deskd/gw/manual_book.cpp
  src/deskd/gw/sys_book.cpp
  src/deskd/gw/write_budget.cpp
  src/deskd/gw/kill.cpp
  src/deskd/gw/kalshi_sign.cpp
  src/deskd/gw/kalshi_wire.cpp
  src/deskd/gw/reject.cpp
  src/deskd/gw/fees.cpp
  src/deskd/strat/strat.cpp
  src/deskd/strat/terminal_price.cpp
  src/deskd/strat/fair.cpp
  src/deskd/strat/maker.cpp
  src/deskd/strat/fill_model.cpp
  src/deskd/strat/calendar.cpp
  src/deskd/strat/lip.cpp
  src/deskd/strat/wcbb_model.cpp
  src/deskd/strat/markouts.cpp
  src/deskd/store/store.cpp
  src/deskd/web/sse.cpp
  src/deskd/web/streams.cpp
  src/deskd/web/contract.cpp
  src/deskd/web/pub.cpp
  src/deskd/web/server.cpp)
# net/ speaks TLS through OpenSSL 3 (Homebrew's openssl@3 on macOS), and gw/ signs with it
target_link_libraries(deskd_core PUBLIC deskd_flags OpenSSL::SSL OpenSSL::Crypto ${DESK_SQLITE_TARGET})
target_precompile_headers(deskd_core PRIVATE
  <nlohmann/json.hpp> <httplib.h>
  <atomic> <chrono> <condition_variable> <deque> <functional> <future> <map> <memory> <mutex>
  <optional> <string> <thread> <variant> <vector>)

# Paths the binaries and tests need, in a generated header rather than -D flags: REUSE_FROM
# needs identical compile flags on every target that shares the PCH.
if(NOT NODE_EXE)
  find_program(NODE_EXE NAMES node)
endif()
set(DESKD_NODE_PATH "")
if(NODE_EXE)
  set(DESKD_NODE_PATH "${NODE_EXE}")
endif()
set(DESKD_PYTHON_PATH "")
if(PYTHON3_EXE)
  set(DESKD_PYTHON_PATH "${PYTHON3_EXE}")
endif()
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/generated/deskd/build_paths.h CONTENT
"#pragma once
// generated by CMakeLists.txt
#define DESKD_SOURCE_DIR \"${CMAKE_SOURCE_DIR}\"
#define DESKD_BIN \"$<TARGET_FILE:deskd>\"
#define DESKCTL_BIN \"$<TARGET_FILE:deskctl>\"
#define FAKE_KALSHI_BIN \"$<TARGET_FILE:fake-kalshi>\"
#define KALSHI_DEMO_PROBE_BIN \"$<TARGET_FILE:kalshi-demo-probe>\"
#define DESKD_NODE \"${DESKD_NODE_PATH}\"
#define DESKD_PYTHON \"${DESKD_PYTHON_PATH}\"
")

add_executable(deskd src/deskd/app/deskd.cpp)
target_link_libraries(deskd PRIVATE deskd_core)
target_precompile_headers(deskd REUSE_FROM deskd_core)

add_executable(deskctl src/deskd/app/deskctl.cpp)
target_link_libraries(deskctl PRIVATE deskd_core)
target_precompile_headers(deskctl REUSE_FROM deskd_core)

# One line registers one suite: a step that adds a suite adds a line and edits no shared list
# (2026-10-21: the automatic lanes are built in parallel worktrees, and every one of them would
# otherwise edit the same foreach). Each suite also joins deskd-all.
#   deskd_pure_test(x)   tests/deskd/test_x.cpp on deskd_core: binary deskd-test-x, ctest deskd-x
#   deskd_fake_test(x)   the same, also linked with the fake exchange (fake_kalshi, below)
#   fake_kalshi_test(x)  tests/deskd/test_fake_x.cpp on the fake alone: deskd-test-fake-x, ctest deskd-fake-x
add_custom_target(deskd-all)
function(deskd_suite target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE ${ARGN})
  target_precompile_headers(${target} REUSE_FROM deskd_core)
  add_dependencies(deskd-all ${target})
endfunction()
function(deskd_pure_test name)
  deskd_suite(deskd-test-${name} tests/deskd/test_${name}.cpp deskd_core)
  add_test(NAME deskd-${name} COMMAND deskd-test-${name})
endfunction()
function(deskd_fake_test name)
  deskd_suite(deskd-test-${name} tests/deskd/test_${name}.cpp deskd_core fake_kalshi)
  add_test(NAME deskd-${name} COMMAND deskd-test-${name})
endfunction()
function(fake_kalshi_test name)
  deskd_suite(deskd-test-fake-${name} tests/deskd/test_fake_${name}.cpp fake_kalshi)
  add_test(NAME deskd-fake-${name} COMMAND deskd-test-fake-${name})
endfunction()

# tests/deskd: the repo's CHECK style, one executable per area.
#   bus      units, messages, the two-lane queue (priority overtakes, a full lane drops and marks a gap)
#   ops      the account lock, the heartbeat rule, config, the subscription and send guards
#   web      Host, origin and token guards, the strict request checker (its verdicts compared
#            with contract.js's), SSE framing and seq, the stream limit and lifecycle, the
#            snapshot and replies checked by contract.js itself (node), KILL with gw frozen and
#            with send-cx's inbox full
#   process  the real binaries: a second deskd refused (also from another working directory),
#            SIGTERM, deskctl status, kill and lane-reset
deskd_pure_test(bus)
deskd_pure_test(ops)
deskd_pure_test(web)
deskd_pure_test(process)
# Without node the contract.js conformance check cannot run: deskd-test-web returns 77 and ctest
# reports the test skipped, not passed (until 2026-10-06 it printed a line and passed).
set_tests_properties(deskd-web PROPERTIES SKIP_RETURN_CODE 77)
add_dependencies(deskd-test-process deskd deskctl)

# ---- tests/fake_exchange: a fake Kalshi for deskd's tests (plan section 8) ---------------------
# The Trade API v2 and its WebSocket on one loopback port, the venue's paths and shapes, a matching
# engine, books from a recorded tape or a scenario, and faults on demand. It shares no code with
# deskd_core (a fake that parsed with deskd's parsers would agree with deskd's mistakes); it only
# reuses deskd_core's precompiled header. fake-kalshi runs it on its own for soak runs.
find_package(ZLIB REQUIRED)
add_library(fake_kalshi STATIC
  tests/fake_exchange/wire.cpp
  tests/fake_exchange/engine.cpp
  tests/fake_exchange/net.cpp
  tests/fake_exchange/rest.cpp
  tests/fake_exchange/tape.cpp
  tests/fake_exchange/fake_kalshi.cpp
  tests/fake_exchange/client.cpp
  tests/fake_exchange/tls_pki.cpp)
target_include_directories(fake_kalshi PUBLIC tests/fake_exchange)
target_link_libraries(fake_kalshi PUBLIC deskd_flags OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
target_precompile_headers(fake_kalshi REUSE_FROM deskd_core)

add_executable(fake-kalshi tests/fake_exchange/main.cpp)
target_link_libraries(fake-kalshi PRIVATE fake_kalshi)
target_precompile_headers(fake-kalshi REUSE_FROM deskd_core)

# The VM recorder's own tests (recorder/tests; tests/test_recorder.py is the old perps recorder's):
# Kalshi, ESPN and NCAA replies parsed from fixtures, the REST pollers on a fake REST, the control
# picks, the gaps report, deploy/base.sh with every system command stubbed, and the recorder run
# against fake-kalshi with loopback scoreboards. No network. A test that needs pyarrow,
# cryptography, websockets or duckdb skips when this python3 has none (recorder/requirements.txt).
if(PYTHON3_EXE)
  add_test(NAME recorder-unit
           COMMAND ${PYTHON3_EXE} -m unittest discover -s recorder/tests -t .
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(recorder-unit PROPERTIES ENVIRONMENT "FAKE_KALSHI=$<TARGET_FILE:fake-kalshi>")
endif()

# The fake's own tests:
#   fake-engine  no sockets: units, grids and fees (the venue's worked rounding example), matching
#                (price-time, IOC, FOK, post-only, expiry, reduce-only, self-trade, pause, close),
#                duplicate client ids, netting and cash per shard, order groups, the socket's
#                message fields, the street and tape debts, the archive reader on recorded frames
#                and the synthetic fixture, scenarios, the fixture played to its end, and 5,000
#                resting orders
#   fake-wire    over loopback: every REST route and shape, cursor paging, auth (a real RSA-PSS
#                signature), rate limits per shard, the socket's commands, sids and seq, every
#                channel, and each fault (drops, 5xx, a cut body, chunked, 429, stalls, socket
#                drops, seq gaps, duplicate fills, the fill before the 201, a cancel racing a fill,
#                a cancel before the 201), the cancel-all's minute of echo, errors 25-27 (buffer
#                overflow, market limit, command rate), one order's private messages kept in order
#                under REPLY_FIRST, the fixture tape over the socket, eight clients at once, and
#                the fake-kalshi binary started and stopped
fake_kalshi_test(engine)
fake_kalshi_test(wire)
add_dependencies(deskd-test-fake-wire fake-kalshi)  # it starts the binary once

# ---- week 1: net/ and the Kalshi wire (plan sections 2-3) ------------------------------------------
#   http   the strict HTTP/1.1 response parser, no sockets: every split of every reply, chunked with
#          extensions and trailers, Content-Length rules, 1xx, no-body statuses, the close-framed
#          body, every refusal (bare LF, folding, TE + CL, oversize ...), and cuts at every byte
#   net    deskd's connections against loopback servers and the fake's TLS listener (a CA made at
#          run time): verification refusals, the guard on every connect, one request per
#          connection, cut and dropped replies as UNKNOWN, close_notify and a bare FIN, idle closes,
#          the spare pool's counters, SO_NOSIGPIPE, latency on loopback
#   wire   the signer (RSA-PSS and Ed25519, verified by the fake), the builders and parsers against
#          the documented shapes and the fake over TLS, what each write reply means, and the demo
#          probe binary run against the fake
deskd_pure_test(http)
deskd_pure_test(pub_data)
deskd_pure_test(order_ledger)
deskd_pure_test(manual_admission)
deskd_pure_test(manual_book)
deskd_pure_test(fair)
deskd_pure_test(espn)
deskd_pure_test(sys_book)
deskd_pure_test(maker)
deskd_pure_test(markouts)
deskd_pure_test(fill)
deskd_pure_test(write_budget)
deskd_pure_test(calendar)
deskd_pure_test(lip)
deskd_pure_test(wcbb)
deskd_pure_test(store)
# sys_risk: gw's sys_used against the loops it replaced, on random books; place_sys's limits at their edges
deskd_pure_test(sys_risk)
deskd_fake_test(net)
deskd_fake_test(wire)
deskd_fake_test(md)
deskd_fake_test(rest)
deskd_fake_test(private)
deskd_fake_test(sender)
deskd_fake_test(manual_gw)
deskd_fake_test(runtime_manual)
deskd_fake_test(pyth)
# fees: gw/fees.h against the fake's model_fee and charge_fee, at the fake's own balance precision
deskd_fake_test(fees)
# sys_gate: gw's lane gates, retirement (LaneMarkout, why RETIRED, deskctl lane-reset) and event cap
deskd_fake_test(sys_gate)
add_dependencies(deskd-test-sender deskctl)   # its deskctl kill --direct case runs the binary
add_dependencies(deskd-test-sys_gate deskctl)   # its lane-reset case runs the binary
# The Systematic soak (tests/deskd/soak.cpp): 60 s under ctest; DESKD_SOAK_SECONDS=14400 for the
# plan's 4 hours, in a build configured with -DDESKD_TSAN=ON.
add_executable(deskd-soak tests/deskd/soak.cpp)
target_link_libraries(deskd-soak PRIVATE deskd_core fake_kalshi)
target_precompile_headers(deskd-soak REUSE_FROM deskd_core)
add_test(NAME deskd-soak COMMAND deskd-soak)
if(DESKD_TSAN)
  # OpenSSL is not instrumented: tests/deskd/tsan.supp says which of its reports are not races
  set_tests_properties(deskd-soak PROPERTIES ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/deskd/tsan.supp")
endif()
set_tests_properties(deskd-soak PROPERTIES TIMEOUT 300)

# tools/kalshi_demo_probe: signed reads (and, asked for, one tiny post-only order and its cancel)
# against Kalshi's DEMO exchange through deskd's own net/ and signer, with latency figures. Not a
# ctest entry (it needs the network and ~/.kalshi/demo.json); deskd-test-wire runs the binary
# against the fake over TLS instead.
add_executable(kalshi-demo-probe tools/kalshi_demo_probe.cpp)
target_link_libraries(kalshi-demo-probe PRIVATE deskd_core)
target_precompile_headers(kalshi-demo-probe REUSE_FROM deskd_core)
add_dependencies(deskd-test-wire kalshi-demo-probe)

# deskd-all: deskd, deskctl, fake-kalshi, the soak and the probe; every suite joined it through its helper
add_dependencies(deskd-all deskd deskctl fake-kalshi deskd-soak kalshi-demo-probe)

add_test(NAME deskd-no-legacy-includes COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/deskd/no_legacy_includes.cmake)

# The plan's invariants (section 8; docs/invariants.md names each rule's test): every suite holding
# one is labelled, so `ctest -L invariants` is the run that must be green before any live order.
# A suite registered only where its runtime exists (node, python3) is labelled where it is.
# One suite a line, as above.
foreach(t
    deskd-bus
    deskd-http
    deskd-manual_admission
    deskd-manual_book
    deskd-manual_gw
    deskd-maker
    deskd-md
    deskd-net
    deskd-ops
    deskd-order_ledger
    deskd-process
    deskd-pub_data
    deskd-pyth
    deskd-rest
    deskd-runtime_manual
    deskd-sender
    deskd-sys_book
    deskd-sys_gate
    deskd-sys_risk
    deskd-web
    deskd-wire
    deskd-write_budget
    recorder-unit
    ui-desk-v3
    ui-desk-v3-manual
    deskd-no-legacy-includes
    )
  if(TEST ${t})
    set_property(TEST ${t} APPEND PROPERTY LABELS invariants)
  endif()
endforeach()
