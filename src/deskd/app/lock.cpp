#include "deskd/app/lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "deskd/app/log.h"

namespace deskd {

std::unique_ptr<AccountLock> AccountLock::acquire(const std::string& data_dir, std::string* err) {
  struct stat st {};
  if (::stat(data_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (err) *err = "data dir " + data_dir + " does not exist (deskd never creates it: the lock must be the shared one)";
    return nullptr;
  }
  const std::string path = data_dir + "/" + kFileName;
  // O_CLOEXEC: a child process must never inherit the lock and keep it after deskd exits.
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = "cannot open " + path + ": " + std::strerror(errno);
    return nullptr;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int e = errno;
    char holder[128] = {0};
    const ssize_t n = ::pread(fd, holder, sizeof holder - 1, 0);
    ::close(fd);
    if (err) {
      std::string who = n > 0 ? std::string(holder, static_cast<std::size_t>(n)) : std::string("unknown");
      while (!who.empty() && (who.back() == '\n' || who.back() == ' ')) who.pop_back();
      *err = e == EWOULDBLOCK ? path + " is held by another process that can send orders (" + who + ")"
                              : "cannot lock " + path + ": " + std::strerror(e);
    }
    return nullptr;
  }
  // Who holds it, for the next process that is refused and for deskctl status.
  char who[128];
  const int len = std::snprintf(who, sizeof who, "pid %d deskd since %s\n", static_cast<int>(::getpid()),
                                iso_ms(now_ms()).c_str());
  if (::ftruncate(fd, 0) == 0 && len > 0) {
    ssize_t w = ::pwrite(fd, who, static_cast<std::size_t>(len), 0);
    (void)w;
  }
  return std::unique_ptr<AccountLock>(new AccountLock(fd, path));
}

AccountLock::~AccountLock() {
  // The file stays: deleting it would let a process that opened the old inode and a process
  // that creates a new one both "hold" the lock.
  if (fd_ >= 0) ::close(fd_);
}

}  // namespace deskd
