#pragma once
// The account lock (plan section 9, week 0): an flock held by every process that can send orders
// to one Kalshi account, so two programs never trade it at once. deskd refuses to start without
// it, and the send guard (net/guard.h) refuses a production host unless deskd holds the prod one.
//
// One fixed absolute file per account, never a path relative to where a program started:
//
//   <lock dir>/kalshi-<env>.lock     env "prod" (deskd --live) or "demo" (deskd otherwise)
//   lock dir: $HOME/Library/Application Support/kalshi-desk/locks, or an absolute "lock_dir" in
//             deskd's config (the tests point it at a temporary directory)
//
// Until 2026-10-06 the file was <data dir>/kalshi-account.lock, and the data dir defaulted to
// "data", relative to the working directory: two deskds started with --data-dir data from the
// main checkout and from the deskd worktree each locked their own file, and both came up (found
// in review). The file is named by the environment, not by the key: two API keys can trade one
// account, so a lock per key would let them both in.
//
// Every order-capable program takes this same file:
//   - deskd (app/deskd.cpp), for its whole life
//   - scripts/account_lock.py on the Python side: desk_supervise.sh runs the old desk's binary
//     under it, `tradeit up` refuses while it is held, kalshi_client.py takes it before its first
//     write
//   - deskctl's direct cancel and tools/demo_checks when they are written (weeks 1-2). A direct
//     cancel runs while a hung deskd still holds the lock, so it cannot take it: its rule is an
//     explicit --live plus the holder line below, which names the holder's mode.
// The holder writes "pid <pid> <holder> since <ISO time>" into the file (deskd: "deskd LIVE" or
// "deskd DEMO"), so a refused process can say who holds it.
#include <memory>
#include <string>

namespace deskd {

class AccountLock {
 public:
  static constexpr const char* kDirUnderHome = "Library/Application Support/kalshi-desk/locks";

  // $HOME/<kDirUnderHome>; "" with *err set when HOME is unset or not absolute
  static std::string default_dir(std::string* err);
  static bool env_ok(const std::string& env);   // "prod" or "demo"

  // Takes the lock on <dir>/kalshi-<env>.lock without waiting. dir must be absolute and exist;
  // create_dir makes it first (for the default dir only: a typo in a configured dir must be a
  // refusal, never a fresh private lock). nullptr with *err set when dir is relative or missing,
  // env is neither prod nor demo, or another process (or another open of the file in this one)
  // holds it; *err then quotes the holder line.
  static std::unique_ptr<AccountLock> acquire(const std::string& dir, const std::string& env, const std::string& holder,
                                              bool create_dir, std::string* err);

  ~AccountLock();
  AccountLock(const AccountLock&) = delete;
  AccountLock& operator=(const AccountLock&) = delete;

  bool held() const { return fd_ >= 0; }
  const std::string& path() const { return path_; }   // the lock file, through realpath(3)
  const std::string& dir() const { return dir_; }     // its directory, through realpath(3)
  const std::string& env() const { return env_; }
  // another per-account file beside the lock: sibling(".killed") = <dir>/kalshi-<env>.killed
  std::string sibling(const std::string& ext) const;

 private:
  AccountLock(int fd, std::string dir, std::string env, std::string path)
      : fd_(fd), dir_(std::move(dir)), env_(std::move(env)), path_(std::move(path)) {}
  int fd_ = -1;
  std::string dir_, env_, path_;
};

}  // namespace deskd
