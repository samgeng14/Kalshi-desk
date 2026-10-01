// Trading invariance of markets_write_mode (2026-10-01). Skip mode stops rewriting market rows that
// did not change, and keeps the poll time in memory instead; the stale-quote gate, the router and
// the systematic screen all read that time as Market.quote_ts. Written by the review that checked
// the change, and kept because it is the test that says skip mode is safe to switch on:
// - differential: the same operations against a Write, a Shadow and a Skip store on one test clock
//   must give every reader on the writer connection byte-identical Markets;
// - readers: on reader connections, a quote_ts read with a book is a time the exchange gave that
//   exact book (it found two books written in the same second sharing one, now fixed);
// - clock_step_back: after a backward clock step skip mode reads what write mode reads.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <filesystem>

#include "desk/store.h"
#include "desk/types.h"

using namespace desk;

static int checks = 0, fails = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++checks;                                                                       \
    if (!(cond)) {                                                                  \
      ++fails;                                                                      \
      if (fails < 30) std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                               \
  } while (0)

static std::string temp_db(const char* name) {
  return (std::filesystem::temp_directory_path() /
          (std::string(name) + "_" + std::to_string(getpid()) + ".sqlite")).string();
}

static std::string sig(const Market& m) {
  json j = market_to_json(m);
  j["__quote_ts"] = m.quote_ts;
  j["__xi"] = m.exchange_index;
  j["__yb"] = m.yes_bid;
  j["__ya"] = m.yes_ask;
  j["__st"] = m.status;
  j["__r2"] = m.rules_secondary;
  return j.dump();
}

static std::string sigs(const std::vector<Market>& v) {
  std::string s;
  for (const auto& m : v) s += sig(m) + "\n";
  return s;
}

static void differential(unsigned seed, int steps) {
  std::mt19937 rng(seed);
  Store W(":memory:"), S(":memory:"), K(":memory:");
  W.set_markets_write_mode(MarketsWriteMode::Write);
  S.set_markets_write_mode(MarketsWriteMode::Shadow);
  K.set_markets_write_mode(MarketsWriteMode::Skip);
  std::vector<Store*> all{&W, &S, &K};
  const int NT = 8;
  std::vector<Market> cur(NT);
  for (int i = 0; i < NT; ++i) {
    Market& m = cur[i];
    m.ticker = std::string(i % 3 ? "KXA" : "KXB") + "-E" + std::to_string(i % 2) + "-B" + std::to_string(i);
    m.series_ticker = i % 3 ? "KXA" : "KXB";
    m.event_ticker = m.series_ticker + "-E" + std::to_string(i % 2);
    m.status = "active";
    m.close_time = "2099-01-01T00:00:00Z";
    m.rules_primary = "r";
    m.yes_bid = 0.40;
    m.yes_ask = 0.42;
  }
  long long now = 1790000000;
  auto set_clock = [&](long long t) { for (auto* s : all) s->set_clock_for_tests(t); };
  auto mutate = [&](Market& m) {
    switch (rng() % 8) {
      case 0: case 1: { static const double v[] = {0.40, 0.41, 0.42}; m.yes_bid = v[rng() % 3]; break; }
      case 2: { static const double v[] = {0.42, 0.43}; m.yes_ask = v[rng() % 2]; break; }
      case 3: m.exchange_index = static_cast<int>(rng() % 2); break;
      case 4: m.rules_secondary = rng() % 2 ? "" : "x"; break;
      case 5: m.volume = static_cast<double>(rng() % 3); break;
      case 6: m.status = rng() % 5 ? "active" : "closed"; break;
      default: m.yes_bid_size = static_cast<double>(rng() % 2); break;
    }
  };
  int mismatches = 0;
  auto compare_all = [&](const char* where, int step) {
    for (const auto& c : cur) {
      auto a = W.get_market(c.ticker), b = S.get_market(c.ticker), k = K.get_market(c.ticker);
      CHECK(bool(a) == bool(b) && bool(a) == bool(k));
      if (a && b && k) {
        const bool ok = sig(*a) == sig(*b) && sig(*a) == sig(*k);
        CHECK(ok);
        if (!ok && ++mismatches < 6)
          std::fprintf(stderr, "  seed %u step %d %s %s: W qts %lld S qts %lld K qts %lld\n", seed, step, where,
                       c.ticker.c_str(), a->quote_ts, b->quote_ts, k->quote_ts);
      }
    }
    for (const char* series : {"", "KXA", "KXB"}) {
      const std::string w = sigs(W.open_markets(series));
      CHECK(w == sigs(S.open_markets(series)));
      CHECK(w == sigs(K.open_markets(series)));
    }
    for (const char* ev : {"KXA-E0", "KXA-E1", "KXB-E0", "KXB-E1"}) {
      const std::string w = sigs(W.markets_in_event(ev));
      CHECK(w == sigs(S.markets_in_event(ev)));
      CHECK(w == sigs(K.markets_in_event(ev)));
    }
  };
  for (int step = 0; step < steps; ++step) {
    const unsigned r = rng() % 100;
    long long d = r < 10 ? 0 : r < 85 ? 1 + rng() % 12 : r < 97 ? 590 + rng() % 30 : 7 * 3600;
    now += d;
    set_clock(now);
    const unsigned op = rng() % 100;
    if (op < 55) {
      // A refresh: some listed markets, sometimes one twice, sometimes rolled back.
      std::vector<Market> batch;
      for (auto& c : cur)
        if (rng() % 4) {
          if (rng() % 4 == 0) mutate(c);
          batch.push_back(c);
          if (rng() % 15 == 0) { Market again = c; if (rng() % 2) mutate(again); batch.push_back(again); c = again; }
        }
      const bool fail = rng() % 20 == 0;
      const int fail_at = batch.empty() ? 0 : static_cast<int>(rng() % batch.size());
      std::vector<std::string> inside[3];
      for (int si = 0; si < 3; ++si) {
        try {
          all[si]->transaction([&] {
            for (int i = 0; i < static_cast<int>(batch.size()); ++i) {
              all[si]->upsert_market(batch[i]);
              // A read inside the transaction (refresh_markets' open_markets(st) does).
              if (auto g = all[si]->get_market(batch[i].ticker)) inside[si].push_back(sig(*g));
              if (fail && i == fail_at) throw std::runtime_error("rollback");
            }
          });
        } catch (const std::exception&) {
        }
      }
      CHECK(inside[0] == inside[1] && inside[0] == inside[2]);
      if (fail) {
        // What the stores hold is the pre-batch state: re-read it into cur.
        for (auto& c : cur)
          if (auto g = W.get_market(c.ticker)) {
            Market keep = c;
            c.yes_bid = g->yes_bid; c.yes_ask = g->yes_ask; c.status = g->status; c.exchange_index = g->exchange_index;
            c.rules_secondary = g->rules_secondary; c.volume = g->volume; c.yes_bid_size = g->yes_bid_size;
            (void)keep;
          }
      }
    } else if (op < 68) {
      Market& c = cur[rng() % NT];
      if (rng() % 2) mutate(c);
      for (auto* s : all) s->upsert_market(c);             // the executor's autocommit upsert
    } else if (op < 76) {
      const std::string t = cur[rng() % NT].ticker;
      for (auto* s : all) s->touch_market(t, now);         // refresh_results' stamp
    } else if (op < 80) {
      for (auto* s : all) s->persist_market_poll_times();
    } else if (op < 86) {
      const long long older = now - static_cast<long long>(rng() % (8 * 3600));
      for (auto* s : all) s->retire_market_poll_times(older);
    } else if (op < 94) {
      // refresh_results: Write/Shadow use the stored updated_ts; Skip filters by effective ts.
      const long long before = now - 6 * 3600 + static_cast<long long>(rng() % 7200);
      std::multiset<long long> w, k;
      for (const auto& row : W.query("SELECT ticker, updated_ts FROM markets WHERE updated_ts<?", {before}))
        w.insert(row["updated_ts"].get<long long>());
      for (const auto& row : K.query("SELECT ticker, updated_ts FROM markets WHERE updated_ts<?", {before})) {
        const long long ts = K.effective_quote_ts(row["ticker"].get<std::string>(), row["updated_ts"].get<long long>());
        if (ts < before) k.insert(ts);
      }
      CHECK(w == k);
    }
    compare_all("after op", step);
  }
}

// Reader connections: a file database, the writer on one thread polling, readers on others. The
// invariant the stale gate needs: a quote_ts read with a book is a time the exchange gave that
// exact book (never a later poll of a different one).
static void readers(unsigned seed, int cycles, bool same_second_writes, int gap_us = 0) {
  const std::string path = temp_db("desk_inv_readers");
  for (const char* sfx : {"", "-wal", "-shm"}) std::remove((path + sfx).c_str());
  Store K(path);
  K.set_markets_write_mode(MarketsWriteMode::Skip);
  const int NT = 4;
  std::vector<Market> cur(NT);
  for (int i = 0; i < NT; ++i) {
    cur[i].ticker = "KXR-E-B" + std::to_string(i);
    cur[i].series_ticker = "KXR";
    cur[i].event_ticker = "KXR-E";
    cur[i].status = "active";
    cur[i].close_time = "2099-01-01T00:00:00Z";
    cur[i].yes_bid = 0.40;
    cur[i].yes_ask = 0.42;
  }
  std::mutex hmu;
  std::map<std::string, std::set<std::pair<long long, double>>> hist;   // ticker -> (poll time, yes_bid)
  std::atomic<bool> stop{false}, ready{false};
  std::atomic<long long> bad{0}, reads{0};
  std::thread writer([&] {
    K.set_writer_thread();
    ready = true;
    std::mt19937 rng(seed);
    long long now = 1790000000;
    for (int c = 0; c < cycles; ++c) {
      now += 1;
      K.set_clock_for_tests(now);
      std::vector<Market> batch;
      for (auto& m : cur) {
        if (rng() % 3 == 0) { static const double v[] = {0.40, 0.41}; m.yes_bid = v[rng() % 2]; }
        batch.push_back(m);
      }
      {
        std::lock_guard<std::mutex> lk(hmu);
        for (const auto& m : batch) hist[m.ticker].insert({now, m.yes_bid});
      }
      K.transaction([&] { for (const auto& m : batch) K.upsert_market(m); });
      if (same_second_writes && rng() % 4 == 0) {
        // A second writer path in the same second (an executor's upsert of a fresh single-market read).
        Market& m = cur[rng() % NT];
        static const double v[] = {0.40, 0.41};
        m.yes_bid = v[rng() % 2];
        {
          std::lock_guard<std::mutex> lk(hmu);
          hist[m.ticker].insert({now, m.yes_bid});
        }
        K.upsert_market(m);
      }
      if (gap_us) std::this_thread::sleep_for(std::chrono::microseconds(gap_us));
    }
    stop = true;
  });
  while (!ready) std::this_thread::yield();
  auto reader = [&] {
    while (!stop) {
      for (const auto& ms : {K.open_markets("KXR"), K.open_markets()}) {
        for (const auto& m : ms) {
          ++reads;
          std::lock_guard<std::mutex> lk(hmu);
          if (!hist[m.ticker].count({m.quote_ts, m.yes_bid})) {
            if (++bad < 5)
              std::fprintf(stderr, "  reader: %s bid %.2f read quote_ts %lld, never polled with that book\n",
                           m.ticker.c_str(), m.yes_bid, m.quote_ts);
          }
        }
      }
    }
  };
  std::thread r1(reader), r2(reader);
  writer.join();
  r1.join();
  r2.join();
  std::printf("  readers(seed %u, same_second=%d, gap %dus): %lld reads, %lld with a quote_ts not a poll of their book\n", seed,
              same_second_writes ? 1 : 0, gap_us, reads.load(), bad.load());
  CHECK(bad == 0);
  for (const char* sfx : {"", "-wal", "-shm"}) std::remove((path + sfx).c_str());
}

// Cost of the refresh transaction for N unchanged rows per mode: the cycle delay before strategies.
static void timing(int n) {
  for (auto mode : {MarketsWriteMode::Write, MarketsWriteMode::Shadow, MarketsWriteMode::Skip}) {
    const std::string path = temp_db("desk_inv_timing");
    for (const char* sfx : {"", "-wal", "-shm"}) std::remove((path + sfx).c_str());
    Store st(path);
    st.set_markets_write_mode(mode);
    std::vector<Market> ms(n);
    for (int i = 0; i < n; ++i) {
      ms[i].ticker = "KXT-E" + std::to_string(i / 20) + "-B" + std::to_string(i);
      ms[i].series_ticker = "KXT" + std::to_string(i % 40);
      ms[i].event_ticker = "KXT-E" + std::to_string(i / 20);
      ms[i].status = "active";
      ms[i].close_time = "2099-01-01T00:00:00Z";
      ms[i].rules_primary = std::string(600, 'r');
      ms[i].yes_bid = 0.4;
      ms[i].yes_ask = 0.42;
    }
    long long now = 1790000000;
    st.set_clock_for_tests(now);
    st.transaction([&] { for (const auto& m : ms) st.upsert_market(m); });
    double best = 1e9;
    for (int k = 0; k < 5; ++k) {
      st.set_clock_for_tests(now + 9 * (k + 1));
      const auto t0 = std::chrono::steady_clock::now();
      st.transaction([&] { for (const auto& m : ms) st.upsert_market(m); });
      best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("  %d unchanged rows, mode %s: refresh transaction %.1f ms (best of 5)\n", n,
                markets_write_mode_name(mode), best);
    for (const char* sfx : {"", "-wal", "-shm"}) std::remove((path + sfx).c_str());
  }
}

// A wall-clock step backwards (an NTP/timed correction): write mode stamps every poll with the new
// clock; skip mode reads max(row, poll), which keeps the pre-step write time, in the future.
static void clock_step_back() {
  Market m;
  m.ticker = "KXC-E-B0"; m.series_ticker = "KXC"; m.event_ticker = "KXC-E"; m.status = "active";
  m.close_time = "2099-01-01T00:00:00Z"; m.yes_bid = 0.40; m.yes_ask = 0.42;
  Store W(":memory:"), K(":memory:");
  W.set_markets_write_mode(MarketsWriteMode::Write);
  K.set_markets_write_mode(MarketsWriteMode::Skip);
  const long long t0 = 1790000000;
  for (auto* s : {&W, &K}) { s->set_clock_for_tests(t0 + 100); s->upsert_market(m); }
  for (auto* s : {&W, &K}) { s->set_clock_for_tests(t0 + 95); s->upsert_market(m); }   // clock stepped back 5 s
  const long long w = W.get_market(m.ticker)->quote_ts, k = K.get_market(m.ticker)->quote_ts;
  std::printf("  clock stepped back 5 s: write-mode quote_ts %+lld, skip-mode quote_ts %+lld (relative to the new now)\n",
              w - (t0 + 95), k - (t0 + 95));
  CHECK(w == k);
}

int main() {
  if (getenv("INV_CLOCK_ONLY")) { clock_step_back(); std::printf("%d checks, %d failures\n", checks, fails); return 0; }
  if (getenv("INV_READERS_ONLY")) {
    readers(8, 4000, true);
    readers(9, 4000, true);
    readers(8, 2000, true, 3000);
    readers(9, 2000, true, 3000);
    readers(10, 2000, false, 0);
    std::printf("%d checks, %d failures\n", checks, fails);
    return 0;
  }
  for (unsigned seed = 1; seed <= 40; ++seed) differential(seed, 1500);
  std::printf("differential: %d checks, %d failures\n", checks, fails);
  readers(7, 4000, false);
  readers(8, 4000, true);
  clock_step_back();
  timing(11000);
  std::printf("%d checks, %d failures\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
