#pragma once
#include "constants.hpp"
#include "websocket.hpp"
#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cctype>
#include <memory>
#include <thread>
#include <vector>

class SpreadManager {
  std::array<net::io_context, NUM_MARKETS> ioc_array_;
  ssl::context ssl_ctx_{ssl::context::tlsv12_client};
  std::vector<Spread> spreads_;
  std::unordered_map<std::string, std::shared_ptr<Leg>> leg_registry_;
  std::array<std::shared_ptr<WebSocketClient>, NUM_MARKETS> clients_;
  std::array<std::vector<std::string>, NUM_MARKETS> symbols_by_market_;
  std::array<std::unordered_map<std::string, std::shared_ptr<Leg>>, NUM_MARKETS>
      callback_maps_;
  std::array<std::thread, NUM_MARKETS> io_threads_;

public:
  SpreadManager(std::vector<Spread> spreads) : spreads_(std::move(spreads)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_none);
  }

  void start() {
    for (auto &spread : spreads_) {
      spread.leg1.ptr =
          get_or_create_leg(spread.leg1.market, spread.leg1.symbol);
      spread.leg2.ptr =
          get_or_create_leg(spread.leg2.market, spread.leg2.symbol);
    }
    // Repeated string allocation on the heap and construction was costly.
    // So construct a map once at initialisation so we can avoid that.
    for (const auto &[key, leg] : leg_registry_) {
      size_t market_idx = market_to_index(leg->market);
      std::string lower_symbol = to_lower(leg->symbol);
      callback_maps_[market_idx][lower_symbol] = leg;
    }
    // One ws per market.
    for (size_t i = 0; i < NUM_MARKETS; ++i) {
      if (!symbols_by_market_[i].empty()) {
        create_combined_stream(static_cast<MarketType>(i),
                               symbols_by_market_[i]);
        io_threads_[i] = std::thread([this, i]() {
          std::cout << "Started I/O thread for market " << i << " ("
                    << market_type_to_string(static_cast<MarketType>(i))
                    << ")\n";
          ioc_array_[i].run();
          std::cout << "I/O thread for market " << i << " finished\n";
        });
      }
    }
  }

  void stop() {
    for (auto &client : clients_)
      if (client)
        client->stop();
    for (size_t i = 0; i < NUM_MARKETS; ++i)
      ioc_array_[i].stop();
    for (size_t i = 0; i < NUM_MARKETS; ++i)
      if (io_threads_[i].joinable())
        io_threads_[i].join();
  }

  const std::vector<Spread> &get_spreads() const { return spreads_; }

  ~SpreadManager() { stop(); }

private:
  std::shared_ptr<Leg> get_or_create_leg(MarketType market,
                                         const std::string &symbol) {
    std::string key;
    key.reserve(market_type_to_string(market).size() + 1 + symbol.size());
    key.append(market_type_to_string(market));
    key.push_back('_');
    key.append(symbol);
    auto it = leg_registry_.find(key);
    if (it != leg_registry_.end())
      return it->second;
    auto leg = std::make_shared<Leg>();
    leg->market = market;
    leg->symbol = symbol;
    leg_registry_[key] = leg;
    size_t market_idx = market_to_index(market);
    symbols_by_market_[market_idx].push_back(to_lower(symbol));
    return leg;
  }

  void create_combined_stream(MarketType market,
                              const std::vector<std::string> &symbols) {
    auto [host, port] = parse_ws_url(get_ws_url(market));
    size_t market_idx = market_to_index(market);
    auto client = std::make_shared<WebSocketClient>(
        ioc_array_[market_idx], ssl_ctx_, host, port, symbols,
        [this, market_idx](const std::string &symbol,
                           const BookTicker &ticker) {
          auto it = callback_maps_[market_idx].find(symbol);
          if (it != callback_maps_[market_idx].end()) {
            it->second->price_data.update(ticker.bid_price, ticker.ask_price,
                                          ticker.timestamp);
          }
        },
        market_idx);
    clients_[market_idx] = client;
    client->run();
  }

  std::pair<std::string, std::string> parse_ws_url(std::string_view url) {
    size_t colon = url.find(':');
    return {std::string(url.substr(0, colon)),
            std::string(url.substr(colon + 1))};
  }

  // Note this should not take reference, since this is passed as a key.
  std::string to_lower(std::string s) {
    for (char &c : s)
      c = std::tolower(static_cast<unsigned char>(c));
    return s;
  }
};
