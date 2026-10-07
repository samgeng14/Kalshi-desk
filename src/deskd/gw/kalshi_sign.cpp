#include "deskd/gw/kalshi_sign.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace deskd {

namespace {

// OpenSSL's reason, without anything from the key itself
std::string ssl_reason() {
  const unsigned long e = ERR_get_error();
  ERR_clear_error();
  if (!e) return "no OpenSSL error";
  const char* r = ERR_reason_error_string(e);
  return r ? r : "OpenSSL error";
}

bool sign_init(EVP_MD_CTX* ctx, EVP_PKEY* pkey, KeyType t, bool verify) {
  EVP_PKEY_CTX* pctx = nullptr;
  if (t == KeyType::ED25519) {
    // pure Ed25519: no digest, the message itself is signed
    return (verify ? EVP_DigestVerifyInit(ctx, &pctx, nullptr, nullptr, pkey)
                   : EVP_DigestSignInit(ctx, &pctx, nullptr, nullptr, pkey)) == 1;
  }
  return (verify ? EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), nullptr, pkey)
                 : EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), nullptr, pkey)) == 1 &&
         EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
         EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) == 1 &&
         EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1;
}

}  // namespace

const char* key_type_name(KeyType t) { return t == KeyType::RSA ? "rsa" : "ed25519"; }

std::string base64_encode(const unsigned char* data, std::size_t n) {
  std::string out(4 * ((n + 2) / 3) + 1, '\0');
  const int k = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(n));
  out.resize(k > 0 ? static_cast<std::size_t>(k) : 0);
  return out;
}

std::string base64_decode(const std::string& s) {
  if (s.empty() || s.size() % 4 != 0) return "";
  std::string out(3 * (s.size() / 4), '\0');
  const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
  if (n < 0) return "";
  std::size_t pad = 0;
  if (s.back() == '=') ++pad;
  if (s.size() > 1 && s[s.size() - 2] == '=') ++pad;
  out.resize(static_cast<std::size_t>(n) - pad);
  return out;
}

std::unique_ptr<KalshiSigner> KalshiSigner::adopt(const std::string& key_id, EVP_PKEY* pkey,
                                                  const std::string& key_type, const std::string& where,
                                                  std::string* err) {
  auto refuse = [&](const std::string& w) -> std::unique_ptr<KalshiSigner> {
    if (pkey) EVP_PKEY_free(pkey);
    if (err) *err = w;
    return nullptr;
  };
  KeyType want;
  if (key_type == "ed25519") want = KeyType::ED25519;
  else if (key_type == "rsa") want = KeyType::RSA;
  else return refuse("key_type must be \"ed25519\" or \"rsa\", not \"" + key_type + "\"");
  if (!pkey) return refuse(where + " holds no private key OpenSSL can read (" + ssl_reason() + ")");
  if (key_id.empty()) return refuse("no key id for " + where);
  const int base = EVP_PKEY_get_base_id(pkey);
  KeyType got;
  if (base == EVP_PKEY_ED25519) got = KeyType::ED25519;
  else if (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) got = KeyType::RSA;
  else return refuse(where + " holds a key that is neither Ed25519 nor RSA");
  if (got != want)
    return refuse(where + " holds an " + std::string(key_type_name(got)) + " key, but the config says " + key_type);
  const int bits = EVP_PKEY_get_bits(pkey);
  if (got == KeyType::RSA && bits < 2048) return refuse(where + " holds an RSA key of " + std::to_string(bits) + " bits");
  // a private key, not a public one in a PEM that also parsed
  if (got == KeyType::ED25519) {
    std::size_t len = 0;
    if (EVP_PKEY_get_raw_private_key(pkey, nullptr, &len) != 1 || len != 32) {
      ERR_clear_error();
      return refuse(where + " holds no Ed25519 private key");
    }
  }
  return std::unique_ptr<KalshiSigner>(new KalshiSigner(key_id, pkey, got, bits));
}

std::unique_ptr<KalshiSigner> KalshiSigner::load(const std::string& key_id, const std::string& pem_path,
                                                 const std::string& key_type, std::string* err) {
  ERR_clear_error();
  BIO* bio = BIO_new_file(pem_path.c_str(), "r");
  if (!bio) {
    ERR_clear_error();
    if (err) *err = "cannot open the private key file " + pem_path;
    return nullptr;
  }
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return adopt(key_id, pkey, key_type, pem_path, err);
}

std::unique_ptr<KalshiSigner> KalshiSigner::from_pem(const std::string& key_id, const std::string& pem,
                                                     const std::string& key_type, std::string* err) {
  ERR_clear_error();
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  EVP_PKEY* pkey = bio ? PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr) : nullptr;
  if (bio) BIO_free(bio);
  return adopt(key_id, pkey, key_type, "the PEM text", err);
}

KalshiSigner::~KalshiSigner() {
  if (pkey_) EVP_PKEY_free(pkey_);
}

std::string KalshiSigner::message(std::int64_t ts_ms, const std::string& method, const std::string& path) {
  const std::size_t q = path.find('?');
  return std::to_string(ts_ms) + method + (q == std::string::npos ? path : path.substr(0, q));
}

std::string KalshiSigner::sign(std::int64_t ts_ms, const std::string& method, const std::string& path) const {
  const std::string msg = message(ts_ms, method, path);
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return "";
  std::string out;
  unsigned char sig[1024];
  std::size_t len = sizeof sig;
  if (sign_init(ctx, pkey_, type_, false) &&
      EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1)
    out = base64_encode(sig, len);
  else
    ERR_clear_error();
  EVP_MD_CTX_free(ctx);
  return out;
}

std::vector<std::pair<std::string, std::string>> KalshiSigner::headers(std::int64_t ts_ms, const std::string& method,
                                                                       const std::string& path) const {
  return {{"KALSHI-ACCESS-KEY", key_id_},
          {"KALSHI-ACCESS-TIMESTAMP", std::to_string(ts_ms)},
          {"KALSHI-ACCESS-SIGNATURE", sign(ts_ms, method, path)}};
}

bool KalshiSigner::verify(std::int64_t ts_ms, const std::string& method, const std::string& path,
                          const std::string& sig_b64) const {
  const std::string msg = message(ts_ms, method, path);
  const std::string sig = base64_decode(sig_b64);
  if (sig.empty()) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  const bool ok = sign_init(ctx, pkey_, type_, true) &&
                  EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                                   reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
  ERR_clear_error();
  EVP_MD_CTX_free(ctx);
  return ok;
}

std::string KalshiSigner::public_pem() const {
  BIO* bio = BIO_new(BIO_s_mem());
  if (!bio) return "";
  std::string out;
  if (PEM_write_bio_PUBKEY(bio, pkey_) == 1) {
    char* p = nullptr;
    const long n = BIO_get_mem_data(bio, &p);
    if (n > 0) out.assign(p, static_cast<std::size_t>(n));
  }
  BIO_free(bio);
  return out;
}

}  // namespace deskd
