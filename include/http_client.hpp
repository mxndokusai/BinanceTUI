#pragma once
#include "types.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <iostream>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

inline std::string get_rest_api_host(MarketType mt) {
  switch (mt) {
  case MarketType::SPOT:
    return "api.binance.com";
  case MarketType::UM_PERP:
    return "fapi.binance.com";
  case MarketType::CM_PERP:
    return "dapi.binance.com";
  }
  return "api.binance.com";
}

inline std::string get_depth_endpoint(MarketType mt) {
  switch (mt) {
  case MarketType::SPOT:
    return "/api/v3/depth";
  case MarketType::UM_PERP:
  case MarketType::CM_PERP:
    return "/fapi/v1/depth";
  }
  return "/api/v3/depth";
}

// Fetch orderbook snapshot via REST API.
inline bool fetch_orderbook_snapshot(MarketType market,
                                     const std::string &symbol,
                                     uint64_t &last_update_id,
                                     std::vector<PriceLevel> &bids,
                                     std::vector<PriceLevel> &asks) {
  try {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_none);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    std::string host = get_rest_api_host(market);

    // Set SNI Hostname.
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
      beast::error_code ec{static_cast<int>(::ERR_get_error()),
                           net::error::get_ssl_category()};
      std::cerr << "SSL set hostname failed: " << ec.message() << "\n";
      return false;
    }

    // Resolve and connect.
    auto const results = resolver.resolve(host, "443");
    beast::get_lowest_layer(stream).connect(results);

    // SSL handshake.
    stream.handshake(ssl::stream_base::client);

    // Build request.
    std::string endpoint = get_depth_endpoint(market);
    std::string target = endpoint + "?symbol=" + symbol + "&limit=5000";

    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    // Send request.
    http::write(stream, req);

    // Receive response.
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // Parse JSON.
    auto json = nlohmann::json::parse(res.body());

    if (!json.contains("lastUpdateId") || !json.contains("bids") ||
        !json.contains("asks")) {
      std::cerr << "Invalid snapshot response for " << symbol << "\n";
      return false;
    }

    last_update_id = json["lastUpdateId"].get<uint64_t>();

    // Parse bids.
    bids.clear();
    for (const auto &bid : json["bids"]) {
      if (bid.is_array() && bid.size() >= 2) {
        PriceLevel level;
        level.price = bid[0].get<std::string>();
        level.quantity = bid[1].get<std::string>();
        bids.push_back(level);
      }
    }

    // Parse asks.
    asks.clear();
    for (const auto &ask : json["asks"]) {
      if (ask.is_array() && ask.size() >= 2) {
        PriceLevel level;
        level.price = ask[0].get<std::string>();
        level.quantity = ask[1].get<std::string>();
        asks.push_back(level);
      }
    }

    // Graceful close.
    beast::error_code ec;
    stream.shutdown(ec);
    // Ignore shutdown errors (common with SSL).
    return true;
  } catch (const std::exception &e) {
    std::cerr << "HTTP fetch error for " << symbol << ": " << e.what() << "\n";
    return false;
  }
}
