#include "tls_pki.h"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sys/stat.h>

#include <cstdio>
#include <memory>

namespace fakex {

namespace {

std::string ssl_err() {
  char buf[256];
  const unsigned long e = ERR_get_error();
  ERR_clear_error();
  if (!e) return "unknown OpenSSL error";
  ERR_error_string_n(e, buf, sizeof buf);
  return buf;
}

EVP_PKEY* new_key() { return EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"); }

bool add_ext(X509* cert, X509* issuer, int nid, const std::string& value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
  if (!ext) return false;
  const bool ok = X509_add_ext(cert, ext, -1) == 1;
  X509_EXTENSION_free(ext);
  return ok;
}

// A certificate for key, signed by issuer_key (issuer: nullptr = self-signed)
X509* make_cert(EVP_PKEY* key, const std::string& cn, X509* issuer, EVP_PKEY* issuer_key, bool ca,
                const std::vector<std::string>& sans, long not_before_s, long not_after_s) {
  X509* c = X509_new();
  if (!c) return nullptr;
  X509_set_version(c, 2);
  unsigned char serial[16];
  RAND_bytes(serial, sizeof serial);
  serial[0] &= 0x7f;
  BIGNUM* bn = BN_bin2bn(serial, sizeof serial, nullptr);
  BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(c));
  BN_free(bn);
  X509_gmtime_adj(X509_getm_notBefore(c), not_before_s);
  X509_gmtime_adj(X509_getm_notAfter(c), not_after_s);
  X509_set_pubkey(c, key);
  X509_NAME* name = X509_get_subject_name(c);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
  X509_set_issuer_name(c, issuer ? X509_get_subject_name(issuer) : name);
  X509* iss = issuer ? issuer : c;
  bool ok = true;
  if (ca) {
    ok = add_ext(c, iss, NID_basic_constraints, "critical,CA:TRUE") &&
         add_ext(c, iss, NID_key_usage, "critical,keyCertSign,cRLSign") &&
         add_ext(c, iss, NID_subject_key_identifier, "hash");
  } else {
    std::string san;
    for (const auto& s : sans) san += (san.empty() ? "" : ",") + s;
    ok = add_ext(c, iss, NID_basic_constraints, "critical,CA:FALSE") &&
         add_ext(c, iss, NID_key_usage, "critical,digitalSignature") &&
         add_ext(c, iss, NID_ext_key_usage, "serverAuth") && (san.empty() || add_ext(c, iss, NID_subject_alt_name, san));
    if (ok && issuer) ok = add_ext(c, iss, NID_authority_key_identifier, "keyid:always");
  }
  if (!ok || X509_sign(c, issuer_key ? issuer_key : key, EVP_sha256()) <= 0) {
    X509_free(c);
    return nullptr;
  }
  return c;
}

bool write_pem(const std::string& path, X509* a, X509* b, EVP_PKEY* key) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  bool ok = true;
  if (a) ok = ok && PEM_write_X509(f, a) == 1;
  if (b) ok = ok && PEM_write_X509(f, b) == 1;
  if (key) ok = ok && PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
  std::fclose(f);
  if (key) ::chmod(path.c_str(), 0600);
  return ok;
}

X509* read_cert(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return nullptr;
  X509* c = PEM_read_X509(f, nullptr, nullptr, nullptr);
  std::fclose(f);
  return c;
}
EVP_PKEY* read_key(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return nullptr;
  EVP_PKEY* k = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
  std::fclose(f);
  return k;
}

}  // namespace

bool make_leaf(const std::string& dir, const std::string& name, const std::string& ca_cert, const std::string& ca_key,
               const std::vector<std::string>& sans, long not_before_s, long not_after_s, std::string* chain_path,
               std::string* key_path, std::string* err) {
  X509* ca = nullptr;
  EVP_PKEY* cak = nullptr;
  if (!ca_cert.empty()) {
    ca = read_cert(ca_cert);
    cak = read_key(ca_key);
    if (!ca || !cak) {
      if (err) *err = "cannot read the CA at " + ca_cert;
      X509_free(ca);
      EVP_PKEY_free(cak);
      return false;
    }
  }
  EVP_PKEY* key = new_key();
  X509* leaf = key ? make_cert(key, name, ca, cak, false, sans, not_before_s, not_after_s) : nullptr;
  const std::string chain = dir + "/" + name + ".pem", kp = dir + "/" + name + ".key";
  const bool ok = leaf && write_pem(chain, leaf, ca, nullptr) && write_pem(kp, nullptr, nullptr, key);
  if (!ok && err) *err = "cannot make leaf " + name + ": " + ssl_err();
  X509_free(leaf);
  X509_free(ca);
  EVP_PKEY_free(key);
  EVP_PKEY_free(cak);
  if (ok) {
    if (chain_path) *chain_path = chain;
    if (key_path) *key_path = kp;
  }
  return ok;
}

bool make_test_pki(const std::string& dir, TestPki* out, std::string* err) {
  EVP_PKEY* cak = new_key();
  X509* ca = cak ? make_cert(cak, "deskd test CA", nullptr, nullptr, true, {}, -60, 86400) : nullptr;
  TestPki p;
  p.dir = dir;
  p.ca_cert = dir + "/ca.pem";
  p.ca_key = dir + "/ca.key";
  bool ok = ca && write_pem(p.ca_cert, ca, nullptr, nullptr) && write_pem(p.ca_key, nullptr, nullptr, cak);
  X509_free(ca);
  EVP_PKEY_free(cak);
  if (!ok) {
    if (err) *err = "cannot make the test CA: " + ssl_err();
    return false;
  }
  if (!make_leaf(dir, "server", p.ca_cert, p.ca_key, {"IP:127.0.0.1", "DNS:localhost"}, -60, 86400, &p.server_chain,
                 &p.server_key, err))
    return false;
  *out = p;
  return true;
}

}  // namespace fakex
