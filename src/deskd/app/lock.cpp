#include "deskd/app/lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "deskd/app/log.h"

namespace deskd {

std::string AccountLock::default_dir(std::string* err) {
  const char* home = std::getenv("HOME");
  if (!home || home[0] != '/') {
    if (err) *err = "HOME is unset or not absolute: no fixed place for the account lock";
    return "";
  }
  return std::string(home) + "/" + kDirUnderHome;
}

bool AccountLock::env_ok(const std::string& env) { return env == "prod" || env == "demo"; }

std::string AccountLock::sibling(const std::string& ext) const { return dir_ + "/kalshi-" + env_ + ext; }

std::unique_ptr<AccountLock> AccountLock::acquire(const std::string& dir, const std::string& env,
                                                  const std::string& holder, bool create_dir, std::string* err) {
  auto refuse = [err](const std::string& why) {
    if (err) *err = why;
    return std::unique_ptr<AccountLock>();
  };
  if (!env_ok(env)) return refuse("account env \"" + env + "\" is neither prod nor demo");
  if (dir.empty() || dir[0] != '/')
    return refuse("lock dir \"" + dir + "\" is not an absolute path: every program must lock the same file");
  if (create_dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return refuse("cannot create lock dir " + dir + ": " + ec.message());
  }
  struct stat st {};
  if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return refuse("lock dir " + dir + " does not exist (a configured lock dir is never created: the lock must be the shared one)");
  char real[PATH_MAX];
  if (!::realpath(dir.c_str(), real)) return refuse("cannot resolve lock dir " + dir + ": " + std::strerror(errno));
  const std::string rdir = real;
  const std::string path = rdir + "/kalshi-" + env + ".lock";
  // O_CLOEXEC: a child process must never inherit the lock and keep it after deskd exits.
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) return refuse("cannot open " + path + ": " + std::strerror(errno));
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int e = errno;
    char who[160] = {0};
    const ssize_t n = ::pread(fd, who, sizeof who - 1, 0);
    ::close(fd);
    std::string line = n > 0 ? std::string(who, static_cast<std::size_t>(n)) : std::string("unknown");
    while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) line.pop_back();
    return refuse(e == EWOULDBLOCK ? path + " is held by another process that can send orders (" + line + ")"
                                   : "cannot lock " + path + ": " + std::strerror(e));
  }
  // Who holds it, for the next process that is refused and for deskctl status.
  char line[256];
  const int len = std::snprintf(line, sizeof line, "pid %d %s since %s\n", static_cast<int>(::getpid()), holder.c_str(),
                                iso_ms(now_ms()).c_str());
  if (::ftruncate(fd, 0) == 0 && len > 0) {
    const std::size_t w = static_cast<std::size_t>(len) < sizeof line ? static_cast<std::size_t>(len) : sizeof line - 1;
    ssize_t r = ::pwrite(fd, line, w, 0);
    (void)r;
  }
  return std::unique_ptr<AccountLock>(new AccountLock(fd, rdir, env, path));
}

AccountLock::~AccountLock() {
  // The file stays: deleting it would let a process that opened the old inode and a process
  // that creates a new one both "hold" the lock.
  if (fd_ >= 0) ::close(fd_);
}

}  // namespace deskd
