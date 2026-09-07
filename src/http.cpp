#include "desk/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace desk {

namespace {
struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};
void ensure_global() {
  static CurlGlobal g;
  (void)g;
}
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* s = static_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

Http::Http(std::string user_agent, long timeout_seconds)
    : user_agent_(std::move(user_agent)), timeout_(timeout_seconds) {
  ensure_global();
}

HttpResponse Http::get(const std::string& url, const std::map<std::string, std::string>& h) const {
  return request("GET", url, "", h);
}
HttpResponse Http::post(const std::string& url, const std::string& body,
                        const std::map<std::string, std::string>& h) const {
  return request("POST", url, body, h);
}
HttpResponse Http::del(const std::string& url, const std::map<std::string, std::string>& h) const {
  return request("DELETE", url, "", h);
}

HttpResponse Http::request(const std::string& method, const std::string& url,
                           const std::string& body,
                           const std::map<std::string, std::string>& headers) const {
  CURL* c = curl_easy_init();
  if (!c) throw std::runtime_error("curl_easy_init failed");
  HttpResponse resp;
  struct curl_slist* hl = nullptr;
  for (const auto& [k, v] : headers) hl = curl_slist_append(hl, (k + ": " + v).c_str());
  if (method == "POST") hl = curl_slist_append(hl, "Content-Type: application/json");
  hl = curl_slist_append(hl, "Accept: application/json");

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent_.c_str());
  curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, std::min(10L, std::max(2L, timeout_ / 2)));
  // A request that carries custom headers is a signed exchange call: never follow a redirect
  // with it (libcurl re-sends custom headers to whatever host the redirect names). Public data
  // sources may redirect (http->https, host moves); allow a few hops there.
  if (headers.empty()) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
  } else {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  }
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https,http");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  if (method == "POST") {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  } else if (method == "DELETE") {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
  }
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) {
    std::string err = curl_easy_strerror(rc);
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    throw std::runtime_error("http " + method + " " + url + ": " + err);
  }
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
  curl_slist_free_all(hl);
  curl_easy_cleanup(c);
  return resp;
}

std::string url_encode(const std::string& s) {
  CURL* c = curl_easy_init();
  char* out = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
  std::string r = out ? out : "";
  curl_free(out);
  curl_easy_cleanup(c);
  return r;
}

}  // namespace desk
