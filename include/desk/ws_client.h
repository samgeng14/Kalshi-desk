#pragma once
// A WebSocket client for the desk's live feeds.
//
// It exists because Kalshi publishes the values its contracts settle on over a socket, not an
// endpoint: the commodity reference feed (`pyth_value`) and the crypto index (`cfbenchmarks_value`)
// are pushed, and polling them would be both slower and a different number. Nothing here is
// commodity-specific: it is a connection that stays up, hands every text frame to a callback on its
// own thread, and says in `status()` whether what it is carrying can be trusted right now.
//
// What it guarantees:
//   - one background thread, never the engine's: `Engine::cycle()` reads a cached value, it never
//     waits for a socket;
//   - reconnect with capped exponential backoff and jitter, re-sending the subscriptions each time;
//   - the auth headers are rebuilt per attempt (a signature carries a timestamp, so a stale one
//     fails the handshake);
//   - ping on a timer, pong on demand, and a silent socket is treated as a dead socket;
//   - `status()` reports connected, the age of the last frame, connects, reconnects and the last
//     error, which is what the router reads before letting anything trade on the feed.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace desk {

using nlohmann::json;

class WsClient {
 public:
  struct Options {
    std::string url;                                  // wss://host[:port]/path (ws:// for tests)
    std::map<std::string, std::string> headers;       // sent on every handshake
    // Rebuilt per connect attempt: a signed header carries a timestamp and cannot be reused.
    std::function<std::map<std::string, std::string>()> auth_headers;
    // Frames sent, in order, immediately after every successful handshake.
    std::function<std::vector<std::string>()> on_connect;
    int ping_interval_seconds = 10;
    int stale_after_seconds = 45;      // no frame for this long: drop the socket and reconnect
    int connect_timeout_seconds = 10;
    double backoff_initial_seconds = 1.0;
    double backoff_max_seconds = 30.0;
    bool verify_tls = true;
    std::string user_agent = "kalshi-desk";
    std::string name = "ws";           // for logs and status
  };
  // (frame text, local receive time in unix milliseconds). Called on the client's own thread.
  using OnText = std::function<void(const std::string&, long long)>;
  // ("connected" | "disconnected" | "error" | "stale", detail). Also on the client's thread.
  using OnEvent = std::function<void(const std::string&, const std::string&)>;

  WsClient(Options options, OnText on_text, OnEvent on_event = {});
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  void start();
  void stop();                       // idempotent; joins the thread
  bool running() const { return running_.load(); }
  // Queue a text frame. Returns false when the socket is not up (the frame is dropped: a
  // subscription is re-sent on connect anyway, and a stale command is worse than no command).
  bool send(const std::string& text);
  json status() const;

  // Exposed for tests: the frame codec and the handshake accept value, which are the two places a
  // WebSocket implementation goes quietly wrong.
  static std::string handshake_accept(const std::string& key_b64);
  static std::string encode_frame(const std::string& payload, int opcode, bool mask);
  // Decodes one frame from `buf`. Returns false when more bytes are needed. Consumes on success.
  static bool decode_frame(std::string& buf, int* opcode, std::string* payload, bool* fin);

 private:
  void run();
  bool connect_once(std::string* why);
  void close_socket();
  bool write_all(const char* data, size_t n);
  int read_some(char* data, size_t n);   // >0 bytes, 0 timeout, -1 closed/error

  Options opt_;
  OnText on_text_;
  OnEvent on_event_;
  std::thread thread_;
  std::atomic<bool> running_{false}, stopping_{false}, connected_{false};

  mutable std::mutex mu_;
  std::vector<std::string> outbox_;
  long long last_message_ms_ = 0, connected_since_ms_ = 0, last_connect_attempt_ms_ = 0;
  long long frames_ = 0, connects_ = 0, reconnects_ = 0, bytes_ = 0;
  std::string last_error_;

  std::string inbox_;        // bytes that arrived with the handshake response
  int fd_ = -1;
  void* ssl_ = nullptr;      // SSL*
  void* ssl_ctx_ = nullptr;  // SSL_CTX*
};

}  // namespace desk
