#pragma once
// The repo's CHECK style (tests/test_main.cpp), shared by the deskd tests, plus a temporary
// directory: deskd's tests never touch data/ or config/, so every lock, heartbeat and config
// file they make lives under $TMPDIR and goes when the test ends.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static int fails = 0, checks = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++checks;                                                                       \
    if (!(cond)) {                                                                  \
      ++fails;                                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                               \
  } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

struct TempDir {
  std::string path;
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "deskd-test-XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) {
      std::perror("mkdtemp");
      std::exit(2);
    }
    path = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string file(const std::string& name) const { return path + "/" + name; }
};

inline int finish(const char* name) {
  std::printf("%s: %d checks, %d failures\n", name, checks, fails);
  return fails == 0 ? 0 : 1;
}
