#include "desk/server.h"

#include <httplib.h>

#include <algorithm>
#include <cstdlib>

#include "desk/eval.h"
#include "desk/risk.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/timeutil.h"
#include "ui_html.h"

namespace desk {

namespace {
void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_header("Cache-Control", "no-store");
  res.set_content(j.dump(), "application/json");
}
json body_json(const httplib::Request& req) {
  if (req.body.empty()) return json::object();
  json j = json::parse(req.body, nullptr, false);
  return j.is_discarded() ? json::object() : j;
}
template <typename F>
void guarded(httplib::Response& res, F&& f) {
  try {
    send_json(res, f());
  } catch (const ApiError& e) {
    send_json(res, json{{"error", e.what()}, {"status", e.status}, {"body", e.body}}, 502);
  } catch (const std::exception& e) {
    send_json(res, json{{"error", e.what()}}, 400);
  }
}
}  // namespace

HttpServer::HttpServer(Engine& engine, Store& store, KalshiClient& client, Executor& exec, int port)
    : engine_(engine), store_(store), client_(client), exec_(exec), port_(port),
      svr_(std::make_unique<httplib::Server>()) {
  routes();
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
  th_ = std::thread([this] {
    LOG_INFO("dashboard on http://127.0.0.1:%d", port_);
    if (!svr_->listen("127.0.0.1", port_)) LOG_ERR("could not bind port %d", port_);
  });
}

void HttpServer::stop() {
  if (svr_) svr_->stop();
  if (th_.joinable()) th_.join();
}

void HttpServer::routes() {
  httplib::Server& s = *svr_;

  // Every mutating route must carry the X-Desk header. A cross-origin page can POST a plain
  // form to 127.0.0.1:8787 without the browser asking anything; it cannot add a custom header
  // without a CORS preflight, which this server never answers. The dashboard adds it on every
  // POST/DELETE.
  s.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
    if ((req.method == "POST" || req.method == "DELETE") && req.path.rfind("/api/", 0) == 0 && !req.has_header("X-Desk")) {
      res.status = 403;
      res.set_content("{\"error\":\"missing X-Desk header (cross-site request refused)\"}", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  s.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(UI_HTML, "text/html; charset=utf-8");
  });

  s.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      json j = engine_.status();
      json pos = json::array();
      for (const auto& p : store_.positions())
        pos.push_back({{"ticker", p.ticker}, {"position", p.position}, {"exposure", p.exposure},
                       {"realized_pnl", p.realized_pnl}, {"fees_paid", p.fees_paid}, {"total_traded", p.total_traded}});
      j["positions"] = pos;
      json ords = json::array();
      for (const auto& o : store_.orders("resting"))
        ords.push_back({{"order_id", o.order_id}, {"ticker", o.ticker}, {"outcome_side", o.outcome_side},
                        {"yes_price", o.yes_price}, {"no_price", o.no_price}, {"remaining", o.remaining_count},
                        {"filled", o.fill_count}, {"created_time", o.created_time}});
      j["orders"] = ords;
      json pend = json::array();
      for (const auto& p : store_.proposals("pending", 100)) {
        json pj = p.to_json();
        if (p.mode == "auto") {
          std::string why;
          pj["auto_ok"] = engine_.auto_rule_ok(p, &why);
          pj["auto_why"] = why;
        }
        if (auto m = store_.get_market(p.ticker)) {
          long long mins = minutes_to_close(m->close_time, m->expected_expiration_time, now_s());
          pj["minutes_to_close"] = mins == kUnknownMinutes ? -1 : mins;
          pj["title"] = m->title;
          pj["sub"] = m->yes_sub_title;
        }
        pend.push_back(pj);
      }
      j["pending"] = pend;
      j["events"] = store_.events(12);
      json recent = json::array();
      for (const auto& p : store_.proposals("", 60)) recent.push_back(p.to_json());
      j["recent"] = recent;
      json fairs = json::array();
      for (const auto& f : store_.fairs())
        fairs.push_back({{"ticker", f.ticker}, {"p", f.p}, {"sigma", f.sigma}, {"note", f.note}, {"ts", f.ts}});
      j["fairs"] = fairs;
      j["config"] = config_to_json(engine_.config());
      j["now"] = now_s();
      return j;
    });
  });

  s.Get("/api/markets", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] { return engine_.markets_view(req.get_param_value("series")); });
  });

  s.Get(R"(/api/orderbook/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      Orderbook b = client_.get_orderbook(req.matches[1], 10);
      store_.insert_book(now_s(), b);
      return orderbook_to_json(b);
    });
  });

  s.Get(R"(/api/quotes/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      long long limit = 500;
      if (req.has_param("limit")) limit = std::stoll(req.get_param_value("limit"));
      return store_.query("SELECT ts,yes_bid,yes_ask,last,volume FROM quotes WHERE ticker=? ORDER BY ts DESC LIMIT ?",
                          {std::string(req.matches[1]), limit});
    });
  });

  s.Get("/api/proposals", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json out = json::array();
      int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 200;
      for (const auto& p : store_.proposals(req.get_param_value("status"), limit)) out.push_back(p.to_json());
      return out;
    });
  });

  s.Post(R"(/api/proposals/(\d+)/approve)", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      json b = body_json(req);
      // The dashboard sends what you confirmed; a proposal the cycle has re-priced since then is
      // refused rather than sent at numbers you never saw.
      return engine_.approve(std::stoll(req.matches[1]), b.value("price", -1.0), b.value("count", -1.0));
    });
  });
  s.Post(R"(/api/proposals/(\d+)/reject)", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      return engine_.reject(std::stoll(req.matches[1]), body_json(req).value("why", ""));
    });
  });

  s.Get("/api/board", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      int limit = req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 400;
      return engine_.board_view(limit);
    });
  });

  // Sizing and hedging read the store and the published snapshot; they must not queue behind a
  // cycle that is out fetching quotes, or the trade strip sits there saying "checking...".
  s.Get("/api/size", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      return engine_.size_suggestion(req.get_param_value("ticker"),
                                     req.has_param("side") ? req.get_param_value("side") : "yes",
                                     std::atof(req.get_param_value("price").c_str()),
                                     req.has_param("style") ? req.get_param_value("style") : "taker");
    });
  });

  s.Get("/api/hedge/book", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] { return engine_.hedge_book_view(); });
  });

  // Hedging. GET plans (payoff table + protective orders), POST places one leg.
  s.Get("/api/hedge", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      auto num = [&](const char* k) { return req.has_param(k) ? std::atof(req.get_param_value(k).c_str()) : 0.0; };
      return engine_.hedge_view(req.get_param_value("event"), req.get_param_value("ticker"),
                                req.has_param("side") ? req.get_param_value("side") : "yes", num("count"), num("price"));
    });
  });

  s.Post("/api/order", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      json b = body_json(req);
      if (engine_.killed()) throw std::runtime_error("kill switch is on");
      OrderRequest r;
      r.ticker = b.value("ticker", "");
      r.outcome_side = b.value("outcome_side", "yes");
      r.price = b.value("price", 0.0);
      r.count = b.value("count", 0.0);
      r.post_only = b.value("post_only", true);
      r.tif = r.post_only ? "good_till_canceled" : b.value("tif", "immediate_or_cancel");
      if (r.ticker.empty() || r.price <= 0 || r.price >= 1 || r.count < .01) throw std::runtime_error("bad ticket");
      Proposal p = exec_.ticket(r, b.value("note", "ticket"));
      engine_.request_reconcile();
      json out = p.to_json();
      // Semi-systematic hedge: the protective leg goes in the same click as the trade it covers.
      // It is sent after the entry so a rejected entry never leaves a naked hedge behind.
      if (b.contains("hedge") && b["hedge"].is_object()) {
        const json& h = b["hedge"];
        OrderRequest hr;
        hr.ticker = h.value("ticker", "");
        hr.outcome_side = h.value("side", "yes");
        hr.price = h.value("price", 0.0);
        hr.count = h.value("count", 0.0);
        hr.post_only = h.value("post_only", false);
        hr.tif = hr.post_only ? "good_till_canceled" : "immediate_or_cancel";
        try {
          if (hr.ticker.empty() || hr.price <= 0 || hr.price >= 1 || hr.count < .01) throw std::runtime_error("bad hedge leg");
          Proposal hp = exec_.ticket(hr, "hedge for " + r.ticker);
          out["hedge"] = hp.to_json();
        } catch (const std::exception& e) {
          out["hedge_error"] = e.what();   // the entry stands; the screen says the hedge did not go
        }
      }
      return out;
    });
  });

  s.Post(R"(/api/orders/([^/]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] { std::lock_guard<std::mutex> lk(engine_.mutex()); return exec_.cancel(req.matches[1]); });
  });

  s.Post(R"(/api/positions/([^/]+)/close)", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json b = body_json(req);
      std::lock_guard<std::mutex> lk(engine_.mutex());
      auto out = exec_.close_position(req.matches[1], b.value("count", 0.0));
      engine_.request_reconcile();
      return out;
    });
  });

  s.Post("/api/fair", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json b = body_json(req);
      FairValue f;
      f.ticker = b.value("ticker", "");
      f.p = b.value("p", 0.0);
      f.sigma = b.value("sigma", 0.05);
      f.note = b.value("note", "");
      if (f.ticker.empty() || f.p <= 0 || f.p >= 1) throw std::runtime_error("need ticker and 0<p<1");
      store_.set_fair(f);
      return json{{"ok", true}};
    });
  });
  s.Delete(R"(/api/fair/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      store_.clear_fair(req.matches[1]);
      return json{{"ok", true}};
    });
  });

  s.Post("/api/kill", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json b = body_json(req);
      if (b.value("on", true)) {
        engine_.request_kill(b.value("reason", "dashboard"));  // no wait on a busy cycle
      } else {
        std::lock_guard<std::mutex> lk(engine_.mutex());
        engine_.set_kill(false, b.value("reason", "dashboard"));
      }
      return json{{"kill", engine_.killed()}};
    });
  });

  // Master switch for unattended sends. Off: auto/bot entries queue like manual ones.
  s.Post("/api/auto", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json b = body_json(req);
      if (b.contains("armed")) engine_.set_auto_armed(b.value("armed", false));
      if (b.contains("instant_edge")) engine_.set_instant_edge(b.value("instant_edge", 0.0));
      return json{{"auto_armed", engine_.auto_armed()}, {"instant_edge", engine_.instant_edge()}};
    });
  });

  // Move cash between exchange shards by hand (e.g. fund the crypto shard).
  s.Post("/api/transfer", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json b = body_json(req);
      int from = b.value("from", 0), to = b.value("to", 2);
      double dollars = b.value("dollars", 0.0);
      if (dollars <= 0 || dollars > 1000 || from == to) throw std::runtime_error("bad transfer");
      std::string id = client_.transfer_between_shards(from, to, dollars);
      store_.log_event("warn", "moved $" + fmt2(dollars) + " from shard " + std::to_string(from) + " to shard " + std::to_string(to) + " (transfer " + id + ")");
      return json{{"transfer_id", id}, {"note", "asynchronous; the shard balance updates on the next sync"}};
    });
  });

  s.Post("/api/uncertain/clear", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      store_.kv_set("uncertain_client_order_id", "");
      store_.kv_set("uncertain_since", "0");
      store_.log_event("warn", "uncertain-order flag cleared by hand");
      return json{{"ok", true}};
    });
  });

  // Brackets: take-profit / stop / trailing stop per position, in prices of the side held or in
  // dollars of P&L (converted with the position's average cost).
  s.Get("/api/brackets", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      json out = json::array();
      auto positions = store_.positions();
      for (const auto& b : store_.query("SELECT * FROM brackets", {})) {
        std::string t = b.value("ticker", "");
        auto pit = std::find_if(positions.begin(), positions.end(), [&](const Position& p) { return p.ticker == t; });
        auto m = store_.get_market(t);
        json row = b;
        if (pit != positions.end() && pit->position != 0) {
          bool long_yes = pit->position > 0;
          double n = std::fabs(pit->position), cost = std::fabs(pit->exposure);
          double bid = m ? (long_yes ? m->yes_bid : (m->no_bid > 0 ? m->no_bid : (m->yes_ask > 0 ? 1.0 - m->yes_ask : 0))) : 0;
          row["side"] = long_yes ? "yes" : "no";
          row["count"] = n;
          row["avg_cost"] = n > 0 ? cost / n : 0;
          row["bid"] = bid;
          row["pnl_now"] = bid > 0 ? bid * n - cost : 0;
          row["pnl_at_tp"] = b.value("tp_price", 0.0) > 0 ? b.value("tp_price", 0.0) * n - cost : 0;
          row["pnl_at_sl"] = b.value("sl_price", 0.0) > 0 ? b.value("sl_price", 0.0) * n - cost : 0;
        }
        out.push_back(row);
      }
      return out;
    });
  });

  s.Post(R"(/api/brackets/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      std::string t = req.matches[1];
      json b = body_json(req);
      // Serialize changes against the cycle. Only bracket-owned orders are affected.
      auto cancel_bracket_orders = [&] {
        for (const auto& o : store_.orders("resting")) {
          if (o.ticker != t) continue;
          auto p = store_.get_proposal(store_.proposal_for_order(o.order_id));
          if (p && (p->strategy == "target" || p->strategy == "stop")) exec_.cancel(o.order_id);
        }
      };
      if (b.value("delete", false)) {
        cancel_bracket_orders();
        store_.exec("DELETE FROM brackets WHERE ticker=?", {t});
        store_.exec("UPDATE proposals SET status='expired',note='bracket removed' WHERE status='pending' AND ticker=? AND strategy IN ('target','stop')", {t});
        store_.log_event("info", "bracket " + t + " removed");
        return json{{"ok", true}};
      }
      auto positions = store_.positions();
      auto pit = std::find_if(positions.begin(), positions.end(), [&](const Position& p) { return p.ticker == t; });
      if (pit == positions.end() || pit->position == 0) throw std::runtime_error("no open position in " + t);
      double n = std::fabs(pit->position), cost = std::fabs(pit->exposure), avg = n > 0 ? cost / n : 0;
      double tp = b.value("tp_price", 0.0), sl = b.value("sl_price", 0.0), trail = b.value("trail", 0.0);
      double tpd = b.value("tp_dollars", 0.0), sld = b.value("sl_dollars", 0.0);
      if (tpd > 0) tp = avg + tpd / n;
      if (sld > 0) sl = avg - sld / n;
      auto m = store_.get_market(t);
      double tick = m && m->tick > 0 ? m->tick : 0.01;
      if (tp > 0) tp = std::min(1.0 - tick, ceil_to_tick(tp, tick));
      if (sl > 0) sl = std::max(tick, floor_to_tick(sl, tick));
      if (tp > 0 && sl > 0 && sl >= tp) throw std::runtime_error("stop must be below target");
      if (tp > 0 && tp <= avg) throw std::runtime_error("target " + fmt2(tp) + " is not above your average cost " + fmt2(avg));
      std::string mode = b.value("mode", "alert");
      if (mode != "alert" && mode != "auto") throw std::runtime_error("mode must be alert or auto");
      for (double value : {tp, sl, trail, tpd, sld})
        if (!std::isfinite(value) || value < 0) throw std::runtime_error("Bracket values must be finite and nonnegative");
      if (tp >= 1 || sl >= 1 || trail >= 1) throw std::runtime_error("Bracket prices and trail must be below 1");
      cancel_bracket_orders();
      long long now = now_s();
      store_.exec(
          "INSERT INTO brackets(ticker,tp_price,sl_price,trail,mode,tp_dollars,sl_dollars,note,created_ts,updated_ts,alerted) "
          "VALUES(?,?,?,?,?,?,?,?,?,?,'') ON CONFLICT(ticker) DO UPDATE SET tp_price=excluded.tp_price, sl_price=excluded.sl_price, "
          "trail=excluded.trail, mode=excluded.mode, tp_dollars=excluded.tp_dollars, sl_dollars=excluded.sl_dollars, "
          "note=excluded.note, updated_ts=excluded.updated_ts, alerted=''",
          {t, tp, sl, trail, mode, tpd, sld, b.value("note", ""), now, now});
      // Any old target order gets re-priced by the next cycle; drop stale pending ones now.
      store_.exec("UPDATE proposals SET status='expired',note='bracket changed' WHERE status='pending' AND ticker=? AND strategy IN ('target','stop')", {t});
      store_.log_event("info", "bracket " + t + ": target " + (tp > 0 ? fmt2(tp) : "-") + " stop " + (sl > 0 ? fmt2(sl) : "-") +
                                   (trail > 0 ? " trail " + fmt2(trail) : "") + " (" + mode + ")");
      return json{{"ticker", t}, {"tp_price", tp}, {"sl_price", sl}, {"trail", trail}, {"mode", mode}, {"avg_cost", avg}, {"count", n}};
    });
  });

  s.Post(R"(/api/strategy/([^/]+)/mode)", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      engine_.set_mode(req.matches[1], body_json(req).value("mode", "off"));
      return engine_.strategies_view();
    });
  });

  s.Post("/api/watch", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::string series = body_json(req).value("series", "");
      if (series.empty()) throw std::runtime_error("need series");
      std::lock_guard<std::mutex> lk(engine_.mutex());
      engine_.add_watch(series);
      return json{{"watch", engine_.watch()}};
    });
  });
  s.Delete(R"(/api/watch/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      engine_.remove_watch(req.matches[1]);
      return json{{"watch", engine_.watch()}};
    });
  });

  // Series search for the "add to watchlist" box. Syncs the exchange's series list hourly.
  s.Get("/api/series", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      long long now = now_s();
      if (now - series_synced_ > 3600) {
        for (const auto& sr : client_.get_series()) store_.upsert_series(sr);
        series_synced_ = now;
      }
      std::string q = req.get_param_value("q"), cat = req.get_param_value("category");
      std::string like = "%" + q + "%";
      if (!cat.empty())
        return store_.query(
            "SELECT ticker,category,title,fee_type,fee_multiplier,frequency FROM series WHERE category=? AND "
            "(ticker LIKE ? OR title LIKE ?) ORDER BY ticker LIMIT 200",
            {cat, like, like});
      return store_.query(
          "SELECT ticker,category,title,fee_type,fee_multiplier,frequency FROM series WHERE ticker LIKE ? OR "
          "title LIKE ? ORDER BY ticker LIMIT 200",
          {like, like});
    });
  });

  s.Get("/api/auto", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      return json{{"buckets", engine_.auto_budget_view()},
                  {"instant_edge", engine_.instant_edge()},
                  {"instant_max_cost", engine_.config().risk.instant_max_cost},
                  {"auto_armed", engine_.auto_armed()},
                  {"spent_today", std::atof(store_.kv_get("auto_spent:" + trading_day(engine_.config().risk.day_tz, now_s())).value_or("0").c_str())},
                  {"day", trading_day(engine_.config().risk.day_tz, now_s())}, {"day_tz", engine_.config().risk.day_tz},
                  {"budget", engine_.config().risk.max_auto_dollars_per_day},
                  {"uncertain_order", store_.kv_get("uncertain_client_order_id").value_or("")}};
    });
  });

  // Per-strategy scorecard for the semi-systematic pipeline: how much research has accumulated,
  // whether the model beats the market on settled markets (paired Brier, 90% CI), and what it
  // has actually made when traded.
  s.Get("/api/scorecard", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      json out = json::object();
      for (const auto& r : store_.query("SELECT strategy, COUNT(*) AS n, COUNT(DISTINCT ticker) AS markets, MIN(ts) AS since FROM model_fairs GROUP BY strategy", {}))
        out[r.value("strategy", "")] = {{"fairs_logged", r.value("n", 0LL)}, {"markets", r.value("markets", 0LL)}, {"since", r.value("since", 0LL)}};
      json rep = build_report(store_, engine_.config());
      if (rep.contains("brier") && rep["brier"].contains("by_strategy"))
        for (auto& [k, v] : rep["brier"]["by_strategy"].items()) out[k]["brier_vs_market"] = v;
      json pnl = build_pnl(store_, engine_.config().risk.day_tz);
      if (pnl.contains("by_strategy"))
        for (auto& [k, v] : pnl["by_strategy"].items()) out[k]["realized"] = v;
      if (rep.contains("realized") && rep["realized"].contains("by_strategy"))
        for (auto& [k, v] : rep["realized"]["by_strategy"].items()) out[k]["edge"] = v;
      return out;
    });
  });

  s.Get("/api/risk_book", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] { return risk_book(store_, engine_.config()); });
  });

  s.Get("/api/risk", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] { return engine_.risk_view(); });
  });

  s.Get("/api/pnl", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] { return build_pnl(store_, engine_.config().risk.day_tz); });
  });

  s.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 100;
      return store_.events(limit);
    });
  });

  // Ladder: live book + our orders + fair + position for one market.
  s.Get(R"(/api/ladder/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      std::string t = req.matches[1];
      json j;
      auto m = store_.get_market(t);
      if (m) {
        j["market"] = market_to_json(*m);
        j["market"]["title"] = m->title;
        j["market"]["rules"] = m->rules_primary;
        if (auto sr = store_.get_series(m->series_ticker)) {
          j["market"]["fee_type"] = sr->fee_type;
          j["market"]["fee_multiplier"] = sr->fee_multiplier;
          j["market"]["category"] = sr->category;
        }
        long long mins = minutes_to_close(m->close_time, m->expected_expiration_time, now_s());
        j["market"]["minutes_to_close"] = mins == kUnknownMinutes ? -1 : mins;
      }
      Orderbook b = client_.get_orderbook(t, 20);
      j["book"] = orderbook_to_json(b);
      json ords = json::array();
      for (const auto& o : store_.orders("resting"))
        if (o.ticker == t)
          ords.push_back({{"order_id", o.order_id}, {"outcome_side", o.outcome_side}, {"yes_price", o.yes_price},
                          {"no_price", o.no_price}, {"remaining", o.remaining_count}});
      j["orders"] = ords;
      if (auto f = engine_.fair_for(t))
        j["fair"] = {{"p", f->first.p}, {"sigma", f->first.sigma}, {"note", f->first.note}, {"strategy", f->second}};
      for (const auto& p : store_.positions())
        if (p.ticker == t) j["position"] = {{"position", p.position}, {"exposure", p.exposure}, {"realized", p.realized_pnl}};
      json pend = json::array();
      for (const auto& p : store_.proposals("pending", 200))
        if (p.ticker == t) pend.push_back(p.to_json());
      j["pending"] = pend;
      return j;
    });
  });

  // Order + fill blotter with slippage vs the proposal price.
  s.Get("/api/blotter", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      long long limit = req.has_param("limit") ? std::stoll(req.get_param_value("limit")) : 150;
      return store_.query(
          "SELECT o.order_id, o.ticker, o.outcome_side, o.status, o.yes_price, o.no_price, o.initial_count,"
          " o.fill_count, o.remaining_count, o.created_time, o.taker_fees, o.maker_fees, o.proposal_id,"
          " p.strategy, p.price AS prop_price, p.p_model, p.exec_style, p.net_edge,"
          " (SELECT SUM(f.count) FROM fills f WHERE f.order_id=o.order_id) AS filled,"
          " (SELECT SUM(f.count*CASE WHEN o.outcome_side='yes' THEN f.yes_price ELSE f.no_price END)/NULLIF(SUM(f.count),0)"
          "   FROM fills f WHERE f.order_id=o.order_id) AS avg_fill,"
          " (SELECT COALESCE(SUM(f.fee),0) FROM fills f WHERE f.order_id=o.order_id) AS fees"
          " FROM orders o LEFT JOIN proposals p ON p.id=o.proposal_id ORDER BY o.created_time DESC LIMIT ?",
          {limit});
    });
  });

  // Live matches the sports strategy is tracking (from ext_sports, last 3 minutes).
  s.Get("/api/sports", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      return store_.query(
          "SELECT * FROM ext_sports WHERE id IN (SELECT MAX(id) FROM ext_sports GROUP BY event_ticker) AND ts>? "
          "ORDER BY ts DESC",
          {now_s() - 180});
    });
  });

  s.Get("/api/wp", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      return store_.query(
          "SELECT * FROM ext_wp WHERE id IN (SELECT MAX(id) FROM ext_wp GROUP BY event_ticker) AND ts>? ORDER BY ts DESC",
          {now_s() - 180});
    });
  });

  s.Get("/api/report", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] { return build_report(store_, engine_.config()); });
  });

  s.Get("/api/hypotheses", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      json out = json::array();
      for (const auto& h : store_.hypotheses())
        out.push_back({{"id", h.id}, {"key", h.key}, {"category", h.category}, {"title", h.title},
                       {"statement", h.statement}, {"data_needed", h.data_needed}, {"test", h.test},
                       {"status", h.status}, {"notes", h.notes}, {"updated_ts", h.updated_ts}});
      return out;
    });
  });
  s.Post(R"(/api/hypotheses/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      json b = body_json(req);
      store_.exec("UPDATE hypotheses SET status=COALESCE(?,status), notes=COALESCE(?,notes), updated_ts=? WHERE key=?",
                  {b.contains("status") ? SqlValue(b["status"].get<std::string>()) : SqlValue(nullptr),
                   b.contains("notes") ? SqlValue(b["notes"].get<std::string>()) : SqlValue(nullptr), now_s(),
                   std::string(req.matches[1])});
      return json{{"ok", true}};
    });
  });

  s.Post("/api/reconcile", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      std::lock_guard<std::mutex> lk(engine_.mutex());
      exec_.reconcile(true);
      return json{{"ok", true}};
    });
  });

  s.Get("/api/fills", [this](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      return store_.query(
          "SELECT f.*, o.proposal_id FROM fills f LEFT JOIN orders o ON o.order_id=f.order_id ORDER BY f.ts DESC LIMIT 200");
    });
  });
}

}  // namespace desk
