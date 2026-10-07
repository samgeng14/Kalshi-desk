#pragma once
// Kalshi request signing (docs.kalshi.com getting_started/api_keys.md, fetched 2026-10-07):
//
//   message   = <timestamp ms> + <METHOD> + <path>   the path with /trade-api/v2 and without the
//                                                    query ("/trade-api/v2/portfolio/orders?limit=5"
//                                                    signs "/trade-api/v2/portfolio/orders")
//   headers   KALSHI-ACCESS-KEY (the key id), KALSHI-ACCESS-TIMESTAMP, KALSHI-ACCESS-SIGNATURE
//             (base64)
//   RSA       RSA-PSS, SHA-256, MGF1-SHA-256, salt = the digest length (ported from src/auth.cpp,
//             which the old desk signed with since 2026-08)
//   Ed25519   pure Ed25519 over the message itself, no prehash; keys are PKCS#8 PEM. The changelog
//             added Ed25519 keys on 2026-10-01; demo accepted one from this account on 2026-10-06.
//
// The key type comes from the config ("ed25519" or "rsa") and must match the PEM: a config that
// says one and a file that holds the other is refused at load, never signed with. RSA keys under
// 2048 bits are refused too.
//
// Nothing here prints or returns the key, and errors name the file, never its contents. The key
// is read straight from the file into OpenSSL (BIO_new_file); deskd keeps it in memory only.
// sign() is const and safe from several threads at once (a fresh EVP_MD_CTX per call over a
// shared, read-only EVP_PKEY).
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

namespace deskd {

enum class KeyType { RSA, ED25519 };
const char* key_type_name(KeyType t);   // "rsa", "ed25519"

class KalshiSigner {
 public:
  // From a PEM file. nullptr with *err when the file cannot be read, holds no private key, or
  // holds a key of another type than key_type ("ed25519" | "rsa").
  static std::unique_ptr<KalshiSigner> load(const std::string& key_id, const std::string& pem_path,
                                            const std::string& key_type, std::string* err);
  // From PEM text (the tests' generated keys).
  static std::unique_ptr<KalshiSigner> from_pem(const std::string& key_id, const std::string& pem,
                                                const std::string& key_type, std::string* err);
  ~KalshiSigner();
  KalshiSigner(const KalshiSigner&) = delete;
  KalshiSigner& operator=(const KalshiSigner&) = delete;

  KeyType type() const { return type_; }
  int bits() const { return bits_; }
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
  std::string public_pem() const;   // the public half, PEM (for the fake's VERIFY mode)

 private:
  KalshiSigner(std::string key_id, EVP_PKEY* pkey, KeyType type, int bits)
      : key_id_(std::move(key_id)), pkey_(pkey), type_(type), bits_(bits) {}
  static std::unique_ptr<KalshiSigner> adopt(const std::string& key_id, EVP_PKEY* pkey, const std::string& key_type,
                                             const std::string& where, std::string* err);
  std::string key_id_;
  EVP_PKEY* pkey_ = nullptr;
  KeyType type_ = KeyType::ED25519;
  int bits_ = 0;
};

std::string base64_encode(const unsigned char* data, std::size_t n);
std::string base64_decode(const std::string& s);   // "" when not base64

}  // namespace deskd
