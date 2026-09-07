#include "desk/auth.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <stdexcept>
#include <vector>

namespace desk {

static std::string ssl_err() {
  char buf[256];
  ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
  return buf;
}

Signer::Signer(std::string key_id, const std::string& pem_path) : key_id_(std::move(key_id)) {
  FILE* f = std::fopen(pem_path.c_str(), "r");
  if (!f) throw std::runtime_error("cannot open private key: " + pem_path);
  pkey_ = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
  std::fclose(f);
  if (!pkey_) throw std::runtime_error("cannot parse private key " + pem_path + ": " + ssl_err());
}

Signer::~Signer() {
  if (pkey_) EVP_PKEY_free(pkey_);
}

std::string Signer::base64(const unsigned char* data, size_t len) {
  std::string out;
  out.resize(4 * ((len + 2) / 3) + 1);
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data, static_cast<int>(len));
  out.resize(n > 0 ? static_cast<size_t>(n) : 0);
  return out;
}

std::string Signer::unbase64(const std::string& s) {
  std::string out;
  out.resize(3 * (s.size() / 4) + 3);
  int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                          reinterpret_cast<const unsigned char*>(s.data()),
                          static_cast<int>(s.size()));
  if (n < 0) return "";
  // EVP_DecodeBlock ignores padding; strip it.
  size_t pad = 0;
  for (auto it = s.rbegin(); it != s.rend() && *it == '='; ++it) ++pad;
  out.resize(static_cast<size_t>(n) - pad);
  return out;
}

std::string Signer::sign(const std::string& text) const {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
  EVP_PKEY_CTX* pctx = nullptr;
  if (EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), nullptr, pkey_) != 1 ||
      EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
      EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("sign init failed: " + ssl_err());
  }
  size_t len = 0;
  if (EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(text.data()),
                     text.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("sign size failed: " + ssl_err());
  }
  std::vector<unsigned char> sig(len);
  if (EVP_DigestSign(ctx, sig.data(), &len, reinterpret_cast<const unsigned char*>(text.data()),
                     text.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("sign failed: " + ssl_err());
  }
  EVP_MD_CTX_free(ctx);
  return base64(sig.data(), len);
}

bool Signer::verify(const std::string& text, const std::string& sig_b64) const {
  std::string sig = unbase64(sig_b64);
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_PKEY_CTX* pctx = nullptr;
  bool ok = EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), nullptr, pkey_) == 1 &&
            EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
            EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1 &&
            EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                             reinterpret_cast<const unsigned char*>(text.data()),
                             text.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

std::map<std::string, std::string> Signer::headers(const std::string& method,
                                                   const std::string& path,
                                                   long long timestamp_ms) const {
  std::string ts = std::to_string(timestamp_ms);
  std::string msg = ts + method + path;
  return {{"KALSHI-ACCESS-KEY", key_id_},
          {"KALSHI-ACCESS-TIMESTAMP", ts},
          {"KALSHI-ACCESS-SIGNATURE", sign(msg)}};
}

}  // namespace desk
