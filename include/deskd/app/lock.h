#pragma once
// The account lock (plan section 9, week 0): flock on <data dir>/kalshi-account.lock, held by
// any process that can send orders, so two programs never trade one account. deskd refuses to
// start without it, and the send guard (net/guard.h) refuses a production host unless it is
// held.
//
// The lock is only as good as its path: every process must lock the same file. deskd therefore
// never creates the data directory; a missing one is a refusal, not a fresh, private lock.
#include <memory>
#include <string>

namespace deskd {

class AccountLock {
 public:
  static constexpr const char* kFileName = "kalshi-account.lock";

  // Takes the lock without waiting. nullptr with *err set when the directory is missing or
  // another process (or another open of the file in this one) holds it; *err then names the
  // holder's pid as it wrote it into the file.
  static std::unique_ptr<AccountLock> acquire(const std::string& data_dir, std::string* err);

  ~AccountLock();
  AccountLock(const AccountLock&) = delete;
  AccountLock& operator=(const AccountLock&) = delete;

  bool held() const { return fd_ >= 0; }
  const std::string& path() const { return path_; }

 private:
  AccountLock(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  int fd_ = -1;
  std::string path_;
};

}  // namespace deskd
