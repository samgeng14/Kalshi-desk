#pragma once
// Minimal libcurl wrapper used for Kalshi and for external data sources.
#include <map>
#include <string>

namespace desk {

struct HttpResponse {
  long status = 0;
  std::string body;
  bool ok() const { return status >= 200 && status < 300; }
};

class Http {
 public:
  explicit Http(std::string user_agent = "kalshi-desk/0.1", long timeout_seconds = 20);
  HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers = {}) const;
  HttpResponse post(const std::string& url, const std::string& body,
                    const std::map<std::string, std::string>& headers = {}) const;
  HttpResponse del(const std::string& url, const std::map<std::string, std::string>& headers = {}) const;

 private:
  HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                       const std::map<std::string, std::string>& headers) const;
  std::string user_agent_;
  long timeout_;
};

std::string url_encode(const std::string& s);

}  // namespace desk
