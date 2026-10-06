#pragma once
// The fake Kalshi's REST routes over its engine: request validation, the venue's JSON shapes,
// filters and cursor paging. No sockets, no locks: the server calls it under its mutex. The paths
// and shapes are listed in fake_kalshi.h.
#include <cstdint>
#include <string>

#include "engine.h"
#include "net.h"

namespace fakex {

struct RestReply {
  int status = 200;
  std::string body;  // "" for a 204
};

// `path` is the request path after /trade-api/v2. `created_order_id` gets the id of an order this
// request created (for the server's HIT_AFTER_REST fault).
RestReply rest_route(Engine& e, const net::Request& req, const std::string& path, const json& body, std::int64_t now_ms,
                     std::string* created_order_id);

RestReply rest_error(int status, const std::string& code, const std::string& message, const std::string& details = "");

// An order as GET /portfolio/orders lists it.
json rest_order_json(const Order& o, const std::string& user_id);

// The cursor the next page starts after, and its parse ("" -> from the top).
std::string make_cursor(std::uint64_t key);
bool read_cursor(const std::string& c, std::uint64_t* key);

}  // namespace fakex
