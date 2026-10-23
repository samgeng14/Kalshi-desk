#include "deskd/gw/poly_sign.h"

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "deskd/gw/kalshi_sign.h"

namespace deskd {

namespace {

constexpr std::size_t kSeed = 32;
constexpr off_t kMaxFile = 4096;   // a secret is 44 or 88 characters; anything this big is something else

bool key_id_ok(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  for (unsigned char c : id)
    if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
  return true;
}

void wipe(std::string& s) {
  if (!s.empty()) OPENSSL_cleanse(s.data(), s.size());
  s.clear();
}

}  // namespace

std::unique_ptr<PolySigner> PolySigner::adopt(const std::string& key_id, std::string secret_b64, const std::string& where,
                                              std::string* err) {
  std::string raw;
  auto refuse = [&](const std::string& w) -> std::unique_ptr<PolySigner> {
    wipe(secret_b64);
    wipe(raw);
    ERR_clear_error();
    if (err) *err = w;
    return nullptr;
  };
  if (!key_id_ok(key_id)) return refuse("the key id for " + where + " must be 1-128 letters, digits, - and _");
  const std::size_t a = secret_b64.find_first_not_of(" \t\r\n");
  const std::size_t b = secret_b64.find_last_not_of(" \t\r\n");
  if (a != std::string::npos) {
    std::string text = secret_b64.substr(a, b - a + 1);
    raw = base64_decode(text);
    wipe(text);
  }
  if (raw.size() != kSeed && raw.size() != 2 * kSeed)
    return refuse(where + " does not hold a base64 secret of 32 or 64 bytes");
  EVP_PKEY* pkey =
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(raw.data()), kSeed);
  if (!pkey) return refuse("OpenSSL could not make an Ed25519 key from " + where);
  if (raw.size() == 2 * kSeed) {
    // seed || public key: the second half must be the seed's own public key
    unsigned char pub[kSeed];
    std::size_t len = sizeof pub;
    const bool same = EVP_PKEY_get_raw_public_key(pkey, pub, &len) == 1 && len == kSeed &&
                      CRYPTO_memcmp(pub, raw.data() + kSeed, kSeed) == 0;
    if (!same) {
      EVP_PKEY_free(pkey);
      return refuse(where + " holds 64 bytes whose second half is not the public key of its first");
    }
  }
  wipe(secret_b64);
  wipe(raw);
  return std::unique_ptr<PolySigner>(new PolySigner(key_id, pkey));
}

std::unique_ptr<PolySigner> PolySigner::load(const std::string& key_id, const std::string& secret_path, std::string* err) {
  std::string path = secret_path;
  if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
    const char* home = std::getenv("HOME");
    if (home && *home) path = std::string(home) + path.substr(1);
  }
  auto refuse = [&](const std::string& w) -> std::unique_ptr<PolySigner> {
    if (err) *err = w;
    return nullptr;
  };
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return refuse("cannot open the Polymarket secret file " + path);
  // the mode of the file opened, not of whatever the path names a moment later
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return refuse(path + " is not a regular file");
  }
  if (st.st_mode & 077) {
    ::close(fd);
    char mode[8];
    std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
    return refuse(path + " may be read by group or others (mode " + mode + "): chmod 600 it");
  }
  if (st.st_size > kMaxFile) {
    ::close(fd);
    return refuse(path + " is too large to be a secret");
  }
  std::string text(static_cast<std::size_t>(kMaxFile), '\0');
  std::size_t got = 0;
  for (;;) {
    const ssize_t n = ::read(fd, text.data() + got, text.size() - got);
    if (n <= 0) break;
    got += static_cast<std::size_t>(n);
    if (got == text.size()) break;
  }
  ::close(fd);
  text.resize(got);   // the bytes past it are still zeros
  return adopt(key_id, std::move(text), path, err);
}

std::unique_ptr<PolySigner> PolySigner::from_secret(const std::string& key_id, const std::string& secret_b64,
                                                    std::string* err) {
  return adopt(key_id, secret_b64, "the secret", err);
}

PolySigner::~PolySigner() {
  if (pkey_) EVP_PKEY_free(pkey_);
}

std::string PolySigner::message(std::int64_t ts_ms, const std::string& method, const std::string& path) {
  const std::size_t q = path.find('?');
  return std::to_string(ts_ms) + method + (q == std::string::npos ? path : path.substr(0, q));
}

std::string PolySigner::sign(std::int64_t ts_ms, const std::string& method, const std::string& path) const {
  const std::string msg = message(ts_ms, method, path);
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return "";
  std::string out;
  unsigned char sig[64];
  std::size_t len = sizeof sig;
  // pure Ed25519: no digest, the message itself is signed
  if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey_) == 1 &&
      EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1)
    out = base64_encode(sig, len);
  else
    ERR_clear_error();
  EVP_MD_CTX_free(ctx);
  return out;
}

std::vector<std::pair<std::string, std::string>> PolySigner::headers(std::int64_t ts_ms, const std::string& method,
                                                                     const std::string& path) const {
  return {{"X-PM-Access-Key", key_id_},
          {"X-PM-Timestamp", std::to_string(ts_ms)},
          {"X-PM-Signature", sign(ts_ms, method, path)}};
}

bool PolySigner::verify(std::int64_t ts_ms, const std::string& method, const std::string& path,
                        const std::string& sig_b64) const {
  const std::string msg = message(ts_ms, method, path);
  const std::string sig = base64_decode(sig_b64);
  if (sig.size() != 64) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  const bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey_) == 1 &&
                  EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                                   reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
  ERR_clear_error();
  EVP_MD_CTX_free(ctx);
  return ok;
}

std::string PolySigner::public_b64() const {
  unsigned char pub[kSeed];
  std::size_t len = sizeof pub;
  if (EVP_PKEY_get_raw_public_key(pkey_, pub, &len) != 1) {
    ERR_clear_error();
    return "";
  }
  return base64_encode(pub, len);
}

}  // namespace deskd
