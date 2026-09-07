#pragma once
// Kalshi request signing: RSA-PSS (SHA-256, salt = digest length) over
//   "<timestamp_ms><METHOD><path-without-query>"
// Headers: KALSHI-ACCESS-KEY, KALSHI-ACCESS-TIMESTAMP, KALSHI-ACCESS-SIGNATURE
#include <map>
#include <memory>
#include <string>

typedef struct evp_pkey_st EVP_PKEY;

namespace desk {

class Signer {
 public:
  Signer(std::string key_id, const std::string& pem_path);
  ~Signer();
  Signer(const Signer&) = delete;
  Signer& operator=(const Signer&) = delete;

  // base64(RSA-PSS-SHA256(text))
  std::string sign(const std::string& text) const;
  // Verifies with the public half of the same key. Used by the self-test.
  bool verify(const std::string& text, const std::string& sig_b64) const;

  // Headers for one request. `path` must include "/trade-api/v2" and no query.
  std::map<std::string, std::string> headers(const std::string& method, const std::string& path,
                                             long long timestamp_ms) const;
  const std::string& key_id() const { return key_id_; }

  static std::string base64(const unsigned char* data, size_t len);
  static std::string unbase64(const std::string& s);

 private:
  std::string key_id_;
  EVP_PKEY* pkey_ = nullptr;
};

}  // namespace desk
