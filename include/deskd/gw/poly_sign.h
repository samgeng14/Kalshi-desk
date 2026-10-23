#pragma once
// Polymarket US request signing (docs/polymarket.md "Wire", ported from src/poly.cpp PolyAuth):
//
//   message   = <timestamp ms> + <METHOD> + <path>   the path without its query
//                                                    ("/v1/portfolio/positions?limit=5" signs
//                                                    "/v1/portfolio/positions")
//   headers   X-PM-Access-Key (the key id), X-PM-Timestamp, X-PM-Signature (base64)
//   Ed25519   pure Ed25519 over the message itself, no prehash
//
// The secret is the base64 the venue shows once when the key is made: the 32-byte seed, or 64
// bytes of seed and public key. A 64-byte secret whose second half is not the seed's public key
// is refused (scripts/check_polymarket.py's layout check): a secret pasted from somewhere else
// would sign, and every request answer 401. The file holding it must be the owner's alone (mode
// 0600 or 0400); one that group or others may read is refused, never used. The key is the
// operator's to make, rotate and provision (decisions 3): agents never ask for it, and the tests
// sign with keys they make themselves.
//
// Nothing here prints, logs or returns the secret, and errors name the file, never its contents.
// The decoded seed is wiped once OpenSSL holds the key. sign() is const and safe from several
// threads at once (a fresh EVP_MD_CTX per call over a shared, read-only EVP_PKEY).
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

namespace deskd {

class PolySigner {
 public:
  // From the secret file: "~/" expands to $HOME; surrounding whitespace is ignored. nullptr with
  // *err when the file cannot be read, may be read by group or others, or holds no secret.
  static std::unique_ptr<PolySigner> load(const std::string& key_id, const std::string& secret_path, std::string* err);
  // From the base64 secret itself (the tests' keys, made at run time).
  static std::unique_ptr<PolySigner> from_secret(const std::string& key_id, const std::string& secret_b64,
                                                 std::string* err);
  ~PolySigner();
  PolySigner(const PolySigner&) = delete;
  PolySigner& operator=(const PolySigner&) = delete;

  const std::string& key_id() const { return key_id_; }

  // The message for a request: ts + METHOD + path, the path cut at its first '?'.
  static std::string message(std::int64_t ts_ms, const std::string& method, const std::string& path);
  // base64 of the signature over message(ts_ms, method, path); "" if OpenSSL fails.
  std::string sign(std::int64_t ts_ms, const std::string& method, const std::string& path) const;
  // The three headers, in the venue's names.
  std::vector<std::pair<std::string, std::string>> headers(std::int64_t ts_ms, const std::string& method,
                                                           const std::string& path) const;
  // With the public half: the tests and the probe's self-check.
  bool verify(std::int64_t ts_ms, const std::string& method, const std::string& path,
              const std::string& sig_b64) const;
  std::string public_b64() const;   // the raw 32-byte public key, base64 (the fake's VERIFY mode)

 private:
  PolySigner(std::string key_id, EVP_PKEY* pkey) : key_id_(std::move(key_id)), pkey_(pkey) {}
  static std::unique_ptr<PolySigner> adopt(const std::string& key_id, std::string secret_b64, const std::string& where,
                                           std::string* err);
  std::string key_id_;
  EVP_PKEY* pkey_ = nullptr;
};

}  // namespace deskd
