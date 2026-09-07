// Dashboard day rollover uses contract dates and excludes expired cached open rows.
static void test_dashboard_refresh() {
  Store st(":memory:");
  const long long now = now_s();
  auto put = [&](const std::string& ticker, const std::string& series, const std::string& closes) {
    Market m; m.ticker=ticker; m.series_ticker=series;
    m.event_ticker=ticker.substr(0,ticker.rfind('-')); m.status="active";
    m.close_time=closes; m.yes_bid=.4; m.yes_ask=.5; st.upsert_market(m);
  };
  put("KXHIGHNY-26SEP06-B75", "KXHIGHNY", iso_of(now+7200));
  put("KXHIGHLAX-26SEP07-B79", "KXHIGHLAX", iso_of(now+86400));
  put("KXHIGHNY-26SEP05-B79", "KXHIGHNY", iso_of(now-1));
  put("KXHIGHNY-26SEP05-B80", "KXHIGHNY", iso_of(now));
  put("KXBTC-26SEP0617-B70000", "KXBTC", iso_of(now+7200));
  put("KXBTC-26SEP0517-B70000", "KXBTC", "2026-01-01T00:00:00-05:00");
  put("KXHIGHMIA-26SEP07-B90", "KXHIGHMIA", "unavailable");
  CHECK(st.open_markets().size()==4);
  CHECK(st.open_markets("KXHIGHNY").size()==1);
  CHECK(st.get_market("KXHIGHNY-26SEP05-B79").has_value()); // retain history
  auto unknown=*st.get_market("KXHIGHMIA-26SEP07-B90");
  unknown.expected_expiration_time=iso_of(now-1);st.upsert_market(unknown);
  CHECK(st.open_markets("KXHIGHMIA").empty());
  Config cfg;cfg.watch_series={"KXHIGHNY","KXHIGHLAX","KXHIGHHOU","KXBTC"};
  KalshiClient client("http://127.0.0.1:1/trade-api/v2",nullptr,"refresh-test");
  Executor ex(cfg,st,client);Engine eng(cfg,st,client,ex);
  auto wx=eng.weather_view();
  CHECK(wx["day"]==trading_day(cfg.risk.day_tz,now_s()));
  CHECK(wx["day_tz"]==cfg.risk.day_tz);
  CHECK(wx["markets"].size()==2); // all watched cities, no crypto or expired history
  CHECK(wx["series"].size()==3); // include cities with no open contracts
  CHECK(wx["counts"]["without_fair"]==2);
  CHECK(wx["markets"][0]["event_date"]=="2026-09-07");
  for(const auto& m:wx["markets"]) {
    CHECK(m["quote_ts"].get<long long>()>=now);
    CHECK(m["quote_stale"]==false);
    CHECK(m["fair_ts"]==0);
    CHECK(m["fair_stale"]==true);
  }
  st.exec("UPDATE markets SET updated_ts=? WHERE series_ticker=?",{now-120,std::string("KXHIGHNY")});
  wx=eng.weather_view();CHECK(wx["counts"]["stale"]==1);
  for(const auto& m:wx["markets"])if(m["series_ticker"]=="KXHIGHNY")CHECK(m["stale"]==true);
  // Explicit research metadata comes from the Fair, not a fragile UI text search.
  Store fair_store(":memory:");seed_kxt(fair_store,.4,.5);
  Config fair_cfg;fair_cfg.watch_series={"KXT"};fair_cfg.strategy_modes["test_model"]="research";
  for(const auto& name:{"weather_nws","crypto_lognormal","sports_inplay","sports_live"})fair_cfg.strategy_modes[name]="off";
  Executor fair_ex(fair_cfg,fair_store,client);Engine fair_engine(fair_cfg,fair_store,client,fair_ex);
  auto test=std::make_unique<TestStrategy>();test->f.p=.45;test->f.sigma=.02;
  fair_engine.add_strategy(std::move(test));fair_engine.cycle();
  auto markets=fair_engine.markets_view("KXT");CHECK(markets.size()==2);
  for(const auto& m:markets) {
    CHECK(m["research_only"]==true);
    CHECK(m["research_reason"]=="strategy in research mode");
    CHECK(m["fair_ts"].get<long long>()>=now);
    CHECK(m["fair_stale"]==false);
  }
  CHECK(!fair_engine.status()["market_feeds"]["KXT"]["last_error"].get<std::string>().empty());
}
