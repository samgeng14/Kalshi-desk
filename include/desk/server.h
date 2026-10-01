#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "desk/client.h"
#include "desk/engine.h"
#include "desk/executor.h"
#include "desk/store.h"

namespace httplib {
class Server;
}

namespace desk {

class HttpServer {
 public:
  HttpServer(Engine& engine, Store& store, KalshiClient& client, Executor& exec, int port);
  ~HttpServer();
  void start();  // spawns the listener thread
  void stop();

 private:
  void routes();
  Engine& engine_;
  Store& store_;
  KalshiClient& client_;
  Executor& exec_;
  int port_;
  std::unique_ptr<httplib::Server> svr_;
  std::thread th_;
  long long series_synced_ = 0;
  // /api/scorecard's model_fairs counts, computed without a sort and kept for 5 minutes.
  json fairs_logged_by_strategy();
  std::mutex scorecard_mu_;
  json scorecard_fairs_ = json::object();
  long long scorecard_ts_ = 0;
};

void seed_hypotheses(Store& store);
// /api/scorecard's research counts per strategy ({fairs_logged, markets, since}), without a sort.
json model_fair_counts(Store& store);

}  // namespace desk
