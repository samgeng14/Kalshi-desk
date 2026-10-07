#pragma once
// Certificates for the fake exchange's TLS listener, made at run time with OpenSSL (EC P-256),
// written as PEM files into a directory the caller owns (the tests' $TMPDIR). Nothing here is
// checked in, and no client turns verification off to talk to the fake: deskd's connection
// factory trusts the test CA for loopback endpoints only (include/deskd/net/conn.h).
//
// make_test_pki writes ca.pem (the root), server.pem (the leaf, then the CA: the chain the
// server sends) and server.key. make_leaf makes the negative cases the client must refuse: a
// leaf for another name, an expired one, one signed by a CA nobody trusts.
#include <string>
#include <vector>

namespace fakex {

struct TestPki {
  std::string dir;
  std::string ca_cert;       // <dir>/ca.pem
  std::string ca_key;        // <dir>/ca.key (for make_leaf)
  std::string server_chain;  // <dir>/server.pem: leaf + CA
  std::string server_key;    // <dir>/server.key
};

// A CA ("deskd test CA", valid one day) and a leaf for 127.0.0.1 and localhost.
bool make_test_pki(const std::string& dir, TestPki* out, std::string* err);

// Another leaf signed by the CA at ca_cert/ca_key (or self-signed when both are ""), for `sans`
// ("IP:127.0.0.1", "DNS:localhost", ...), valid from now + not_before_s to now + not_after_s.
// Writes <dir>/<name>.pem (leaf + CA) and <dir>/<name>.key.
bool make_leaf(const std::string& dir, const std::string& name, const std::string& ca_cert,
               const std::string& ca_key, const std::vector<std::string>& sans, long not_before_s,
               long not_after_s, std::string* chain_path, std::string* key_path, std::string* err);

}  // namespace fakex
