// deskd gw/poly_sign: Polymarket US's request signer.
//
//   - an Ed25519 known answer (RFC 8032 key 1, computed with Python's cryptography 50.0.1), the
//     query never signed, the X-PM-* headers
//   - 32- and 64-byte secrets, a 64-byte secret with another key's public half, secrets that are
//     not one, a key id that could split a header
//   - the file: the owner's alone (0600, 0400) or refused (0644, 0640, 0604), a missing file, a
//     directory, one too large, "~/" as $HOME's, and no secret in any error
//   - signatures checked by OpenSSL with the public half alone, and four threads signing at once
#include <openssl/evp.h>
#include <sys/stat.h>

#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/poly_sign.h"

using namespace deskd;

namespace {

const std::string KEY_ID = "a422f5a3-0000-4000-8000-000000000000";
// RFC 8032 section 7.1 test 1's secret key: a published test vector, not a credential
const std::string RFC_SEED = "nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A=";
const std::string RFC_PUB = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";

}  // namespace

// ---- the signer --------------------------------------------------------------------------------

namespace {
// a key made here, its seed and public half (raw)
struct NewKey {
  std::string seed, pub;
  NewKey() {
    EVP_PKEY* k = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    unsigned char b[32];
    std::size_t n = sizeof b;
    EVP_PKEY_get_raw_private_key(k, b, &n);
    seed.assign(reinterpret_cast<const char*>(b), n);
    n = sizeof b;
    EVP_PKEY_get_raw_public_key(k, b, &n);
    pub.assign(reinterpret_cast<const char*>(b), n);
    EVP_PKEY_free(k);
  }
};
std::string b64(const std::string& raw) { return base64_encode(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()); }
// OpenSSL directly, with the public half alone: not PolySigner::verify
bool verify_raw(const std::string& pub, const std::string& msg, const std::string& sig_b64) {
  const std::string sig = base64_decode(sig_b64);
  EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(pub.data()),
                                            pub.size());
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const bool ok = k && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
                  EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                                   reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(k);
  return ok;
}
void write_file(const std::string& path, const std::string& text, mode_t mode) {
  std::ofstream(path) << text;
  ::chmod(path.c_str(), mode);
}
}  // namespace

static void test_signer() {
  std::string err;
  // known answers (RFC 8032 key 1; Python's cryptography 50.0.1 signed the same messages, and gives
  // the RFC's own signature for the empty one)
  auto kat = PolySigner::from_secret("kat", RFC_SEED, &err);
  CHECK(kat != nullptr);
  if (kat) {
    CHECK(kat->public_b64() == RFC_PUB && kat->key_id() == "kat");
    CHECK(PolySigner::message(1788900000000, "GET", "/v1/portfolio/positions?limit=200") ==
          "1788900000000GET/v1/portfolio/positions");
    CHECK(kat->sign(1788900000000, "GET", "/v1/portfolio/positions") ==
          "+XREw/Q+9/k8M5M77twNp4YhubrvmnNHlM6370yEeSnWSY0XlWQ0fz9h1IqfWGYjWpWkeqmEO3EelHmiRn/YDg==");
    // the query is never signed
    CHECK(kat->sign(1788900000000, "GET", "/v1/portfolio/positions?limit=200&cursor=x") ==
          kat->sign(1788900000000, "GET", "/v1/portfolio/positions"));
    CHECK(kat->sign(1788900000123, "POST", "/v1/orders") ==
          "CjGt93bbY5taBQ9Kc/jwSXpaPkaf9Sw7VuhTzQ8cU9ajGYX2EYMG4I1r3HB2XSLpu8MxCgQEpGryEPn4XGznCQ==");
    const auto h = kat->headers(1788900000123, "POST", "/v1/orders");
    CHECK(h.size() == 3 && h[0] == std::make_pair(std::string("X-PM-Access-Key"), std::string("kat")) &&
          h[1] == std::make_pair(std::string("X-PM-Timestamp"), std::string("1788900000123")) &&
          h[2].first == "X-PM-Signature" && h[2].second == kat->sign(1788900000123, "POST", "/v1/orders"));
    CHECK(kat->verify(1788900000123, "POST", "/v1/orders", h[2].second));
    CHECK(!kat->verify(1788900000124, "POST", "/v1/orders", h[2].second));
    CHECK(!kat->verify(1788900000123, "GET", "/v1/orders", h[2].second));
    CHECK(!kat->verify(1788900000123, "POST", "/v1/orders", "not base64!"));
  }

  // a key made here: its signatures verify under OpenSSL with the public half alone
  const NewKey nk;
  auto s = PolySigner::from_secret(KEY_ID, b64(nk.seed), &err);
  CHECK(s && s->public_b64() == b64(nk.pub));
  if (s) {
    const std::string sig = s->sign(1788900000000, "DELETE", "/v1/order/x");
    CHECK(sig.size() == 88 && verify_raw(nk.pub, "1788900000000DELETE/v1/order/x", sig));
  }
  // 64 bytes, seed then public half: the same key; another key's public half is refused
  auto s64 = PolySigner::from_secret(KEY_ID, b64(nk.seed + nk.pub), &err);
  CHECK(s64 && s64->public_b64() == b64(nk.pub));
  const NewKey other;
  const std::string mixed = b64(nk.seed + other.pub);
  CHECK(!PolySigner::from_secret(KEY_ID, mixed, &err) && err.find("second half") != std::string::npos &&
        err.find(mixed.substr(0, 20)) == std::string::npos && err.find(b64(nk.seed).substr(0, 20)) == std::string::npos);
  CHECK(!PolySigner::from_secret(KEY_ID, b64(nk.seed.substr(0, 31)), &err) && err.find("32 or 64 bytes") != std::string::npos);
  CHECK(!PolySigner::from_secret(KEY_ID, "not-a-key", &err) && err.find("32 or 64 bytes") != std::string::npos);
  CHECK(!PolySigner::from_secret(KEY_ID, "", &err));
  CHECK(!PolySigner::from_secret("", b64(nk.seed), &err) && err.find("key id") != std::string::npos);
  CHECK(!PolySigner::from_secret("k\r\nX-Other: 1", b64(nk.seed), &err));   // nothing that splits a header
  CHECK(PolySigner::from_secret(KEY_ID, "  " + b64(nk.seed) + "\n", &err) != nullptr);

  // the file: the owner's alone, else refused, and never quoted
  TempDir d;
  const std::string secret = b64(nk.seed);
  write_file(d.file("pm.secret"), secret + "\n", 0600);
  auto f = PolySigner::load(KEY_ID, d.file("pm.secret"), &err);
  CHECK(f && f->public_b64() == b64(nk.pub));
  write_file(d.file("pm.secret"), secret + "\n", 0400);
  CHECK(PolySigner::load(KEY_ID, d.file("pm.secret"), &err) != nullptr);
  ::chmod(d.file("pm.secret").c_str(), 0644);
  CHECK(!PolySigner::load(KEY_ID, d.file("pm.secret"), &err) && err.find("mode 0644") != std::string::npos &&
        err.find("chmod 600") != std::string::npos && err.find(secret.substr(0, 20)) == std::string::npos);
  ::chmod(d.file("pm.secret").c_str(), 0640);
  CHECK(!PolySigner::load(KEY_ID, d.file("pm.secret"), &err) && err.find("mode 0640") != std::string::npos);
  ::chmod(d.file("pm.secret").c_str(), 0604);
  CHECK(!PolySigner::load(KEY_ID, d.file("pm.secret"), &err));
  CHECK(!PolySigner::load(KEY_ID, d.file("missing"), &err) &&
        err == "cannot open the Polymarket secret file " + d.file("missing"));
  CHECK(!PolySigner::load(KEY_ID, d.path, &err) && err.find("not a regular file") != std::string::npos);
  write_file(d.file("junk"), "this is not a secret at all", 0600);
  CHECK(!PolySigner::load(KEY_ID, d.file("junk"), &err) && err.find("32 or 64 bytes") != std::string::npos &&
        err.find("not a secret") == std::string::npos);
  write_file(d.file("big"), std::string(5000, 'A'), 0600);
  CHECK(!PolySigner::load(KEY_ID, d.file("big"), &err) && err.find("too large") != std::string::npos);
  write_file(d.file("mixed"), mixed, 0600);
  CHECK(!PolySigner::load(KEY_ID, d.file("mixed"), &err) && err.find(d.file("mixed")) != std::string::npos &&
        err.find(mixed.substr(0, 20)) == std::string::npos);
  // "~/" is $HOME's
  const char* home = std::getenv("HOME");
  const std::string saved = home ? home : "";
  ::setenv("HOME", d.path.c_str(), 1);
  write_file(d.file("pm.secret"), secret, 0600);
  CHECK(PolySigner::load(KEY_ID, "~/pm.secret", &err) != nullptr);
  if (home) ::setenv("HOME", saved.c_str(), 1);
  else ::unsetenv("HOME");

  // four threads signing at once over one key
  if (s) {
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
      ts.emplace_back([&, t] {
        for (int i = 0; i < 200; ++i) {
          const std::string p = "/v1/order/o" + std::to_string(t);
          if (!s->verify(i, "GET", p, s->sign(i, "GET", p))) ++bad;
        }
      });
    for (auto& t : ts) t.join();
    CHECK(bad == 0);
  }
}

int main() {
  test_signer();
  return finish("deskd-poly_wire");
}
