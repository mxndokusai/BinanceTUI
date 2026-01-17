#pragma once
#include "constants.hpp"
#include "http_client.hpp"
#include "websocket.hpp"
#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cctype>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

class DataManager {
  std::array<net::io_context, NUM_MARKETS> ioc_array_;
  ssl::context ssl_ctx_{ssl::context::tlsv12_client};

  // Spreads tracking.
  std::vector<Spread> spreads_;
  std::unordered_map<std::string, std::shared_ptr<Leg>> leg_registry_;
  std::array<std::shared_ptr<WebSocketClient>, NUM_MARKETS> spread_clients_;
  std::array<std::vector<std::string>, NUM_MARKETS> spread_symbols_by_market_;
  std::array<std::unordered_map<std::string, std::shared_ptr<Leg>>, NUM_MARKETS>
      spread_callback_maps_;

  // Orderbook tracking.
  std::vector<OrderBookConfig> orderbook_configs_;
  std::unordered_map<std::string, std::shared_ptr<Leg>> orderbook_registry_;
  std::array<std::shared_ptr<WebSocketClient>, NUM_MARKETS> orderbook_clients_;
  std::array<std::vector<std::string>, NUM_MARKETS>
      orderbook_symbols_by_market_;
  std::array<std::unordered_map<std::string, std::shared_ptr<Leg>>, NUM_MARKETS>
      orderbook_callback_maps_;

  std::array<std::thread, NUM_MARKETS> io_threads_;

public:
  DataManager(std::vector<Spread> spreads,
              std::vector<OrderBookConfig> orderbooks)
      : spreads_(std::move(spreads)),
        orderbook_configs_(std::move(orderbooks)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_none);
  }

  void start() {
    // Setup spreads.
    if (!spreads_.empty()) {
      setup_spreads();
    }
    // Setup orderbooks.
    if (!orderbook_configs_.empty()) {
      setup_orderbooks();
    }
    // Start I/O threads for each market that has connections.
    for (size_t i = 0; i < NUM_MARKETS; ++i) {
      if (!spread_symbols_by_market_[i].empty() ||
          !orderbook_symbols_by_market_[i].empty()) {
        io_threads_[i] = std::thread([this, i]() {
          spdlog::info("Started I/O thread for market {} ({})", i,
                       market_type_to_string(static_cast<MarketType>(i)));
          ioc_array_[i].run();
          spdlog::info("I/O thread for market {} finished", i);
        });
      }
    }
  }

  void stop() {
    for (auto &client : spread_clients_)
      if (client)
        client->stop();
    for (auto &client : orderbook_clients_)
      if (client)
        client->stop();
    for (size_t i = 0; i < NUM_MARKETS; ++i)
      ioc_array_[i].stop();
    for (size_t i = 0; i < NUM_MARKETS; ++i)
      if (io_threads_[i].joinable())
        io_threads_[i].join();
  }

  const std::vector<Spread> &get_spreads() const { return spreads_; }

  const std::vector<OrderBookConfig> &get_orderbook_configs() const {
    return orderbook_configs_;
  }

  std::shared_ptr<OrderBook> get_orderbook(const std::string &market_symbol) {
    auto it = orderbook_registry_.find(market_symbol);
    if (it != orderbook_registry_.end() && it->second->orderbook) {
      return it->second->orderbook;
    }
    return nullptr;
  }

  ~DataManager() { stop(); }

private:
  void setup_spreads() {
    // Create legs for spreads.
    for (auto &spread : spreads_) {
      spread.leg1.ptr =
          get_or_create_spread_leg(spread.leg1.market, spread.leg1.symbol);
      spread.leg2.ptr =
          get_or_create_spread_leg(spread.leg2.market, spread.leg2.symbol);
    }

    // Build callback maps.
    for (const auto &[key, leg] : leg_registry_) {
      size_t market_idx = market_to_index(leg->market);
      std::string lower_symbol = to_lower(leg->symbol);
      spread_callback_maps_[market_idx][lower_symbol] = leg;
    }

    // Create WebSocket connections for spreads (bookTicker)
    for (size_t i = 0; i < NUM_MARKETS; ++i) {
      if (!spread_symbols_by_market_[i].empty()) {
        create_spread_stream(static_cast<MarketType>(i),
                             spread_symbols_by_market_[i]);
      }
    }
  }

  void setup_orderbooks() {
    // Create legs with orderbooks.
    for (const auto &config : orderbook_configs_) {
      auto leg = get_or_create_orderbook_leg(config.market, config.symbol);
      leg->orderbook = std::make_shared<OrderBook>();
    }

    // Build callback maps.
    for (const auto &[key, leg] : orderbook_registry_) {
      size_t market_idx = market_to_index(leg->market);
      std::string lower_symbol = to_lower(leg->symbol);
      orderbook_callback_maps_[market_idx][lower_symbol] = leg;
    }

    // Create WebSocket connections for orderbooks (depth).
    for (size_t i = 0; i < NUM_MARKETS; ++i) {
      if (!orderbook_symbols_by_market_[i].empty()) {
        create_orderbook_stream(static_cast<MarketType>(i),
                                orderbook_symbols_by_market_[i]);
      }
    }
  }

  std::shared_ptr<Leg> get_or_create_spread_leg(MarketType market,
                                                const std::string &symbol) {
    std::string key = make_key(market, symbol);
    auto it = leg_registry_.find(key);
    if (it != leg_registry_.end())
      return it->second;

    auto leg = std::make_shared<Leg>();
    leg->market = market;
    leg->symbol = symbol;
    leg_registry_[key] = leg;

    size_t market_idx = market_to_index(market);
    spread_symbols_by_market_[market_idx].push_back(to_lower(symbol));
    return leg;
  }

  std::shared_ptr<Leg> get_or_create_orderbook_leg(MarketType market,
                                                   const std::string &symbol) {
    std::string key = make_key(market, symbol);
    auto it = orderbook_registry_.find(key);
    if (it != orderbook_registry_.end())
      return it->second;

    auto leg = std::make_shared<Leg>();
    leg->market = market;
    leg->symbol = symbol;
    orderbook_registry_[key] = leg;

    size_t market_idx = market_to_index(market);
    orderbook_symbols_by_market_[market_idx].push_back(to_lower(symbol));
    return leg;
  }

  void create_spread_stream(MarketType market,
                            const std::vector<std::string> &symbols) {
    auto [host, port] = parse_ws_url(get_ws_url(market));
    size_t market_idx = market_to_index(market);

    auto client = std::make_shared<WebSocketClient>(
        ioc_array_[market_idx], ssl_ctx_, host, port, symbols,
        StreamType::BOOK_TICKER,
        // BookTicker callback.
        [this, market_idx](const std::string &symbol,
                           const BookTicker &ticker) {
          auto it = spread_callback_maps_[market_idx].find(symbol);
          if (it != spread_callback_maps_[market_idx].end()) {
            it->second->price_data.update(ticker.bid_price, ticker.ask_price,
                                          ticker.timestamp);
          }
        },
        // Depth callback (unused for spreads).
        nullptr, market_idx);

    spread_clients_[market_idx] = client;
    client->run();
  }

  void create_orderbook_stream(MarketType market,
                               const std::vector<std::string> &symbols) {
    auto [host, port] = parse_ws_url(get_ws_url(market));
    size_t market_idx = market_to_index(market);

    auto client = std::make_shared<WebSocketClient>(
        ioc_array_[market_idx], ssl_ctx_, host, port, symbols,
        StreamType::DEPTH,
        // BookTicker callback (unused for orderbooks).
        nullptr,
        // Depth callback that implements Binance's sync algorithm.
        [this, market_idx, market](const std::string &symbol,
                                   const DepthUpdate &update) {
          auto it = orderbook_callback_maps_[market_idx].find(symbol);
          if (it == orderbook_callback_maps_[market_idx].end() ||
              !it->second->orderbook) {
            return;
          }

          auto leg = it->second;
          auto &orderbook = leg->orderbook;
          auto state = orderbook->get_state();

          if (state == OrderBook::State::UNINITIALISED) {
            // Start buffering and fetch snapshot in background thread.
            {
              std::lock_guard<std::mutex> lock(leg->buffer_mutex);
              leg->buffered_updates.push_back(update);
            }

            // Fetch snapshot only once.
            std::thread([leg, market, symbol]() {
              uint64_t last_update_id;
              std::vector<PriceLevel> bids, asks;

              std::string upper_symbol = symbol;
              for (char &c : upper_symbol)
                c = std::toupper(static_cast<unsigned char>(c));

              if (fetch_orderbook_snapshot(market, upper_symbol, last_update_id,
                                           bids, asks)) {
                leg->orderbook->init_from_snapshot(last_update_id, bids, asks);

                // Process buffered events.
                std::vector<DepthUpdate> buffered_copy;
                {
                  std::lock_guard<std::mutex> lock(leg->buffer_mutex);
                  buffered_copy = std::move(leg->buffered_updates);
                  leg->buffered_updates.clear();
                }

                if (!leg->orderbook->process_buffered_events(buffered_copy)) {
                  std::cerr << "Failed to sync orderbook for " << symbol
                            << ", will retry\n";
                }
              } else {
                std::cerr << "Failed to fetch snapshot for " << symbol << "\n";
              }
            }).detach();

          } else if (state == OrderBook::State::BUFFERING) {
            // Waiting for snapshot processing.
            std::lock_guard<std::mutex> lock(leg->buffer_mutex);
            leg->buffered_updates.push_back(update);

          } else if (state == OrderBook::State::SYNCHRONISED) {
            // Normal operation, apply update.
            if (!orderbook->apply_update(update)) {
              std::cerr << "Orderbook out of sync for " << symbol
                        << ", reinitialising\n";
            }
          }
        },
        market_idx + 100); // Offset ID to distinguish from spread clients.

    orderbook_clients_[market_idx] = client;
    client->run();
  }

  std::pair<std::string, std::string> parse_ws_url(std::string_view url) {
    size_t colon = url.find(':');
    return {std::string(url.substr(0, colon)),
            std::string(url.substr(colon + 1))};
  }

  std::string to_lower(std::string s) {
    for (char &c : s)
      c = std::tolower(static_cast<unsigned char>(c));
    return s;
  }

  std::string make_key(MarketType market, const std::string &symbol) {
    std::string key;
    key.reserve(market_type_to_string(market).size() + 1 + symbol.size());
    key.append(market_type_to_string(market));
    key.push_back('_');
    key.append(symbol);
    return key;
  }
};
