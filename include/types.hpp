#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

enum class MarketType : uint8_t { SPOT = 0, UM_PERP, CM_PERP };

inline constexpr std::string_view market_type_to_string(MarketType mt) {
  switch (mt) {
  case MarketType::SPOT:
    return "SPOT";
  case MarketType::UM_PERP:
    return "UM_FUTURES";
  case MarketType::CM_PERP:
    return "CM_FUTURES";
  }
  return "UNKNOWN";
}

inline constexpr std::string_view get_ws_url(MarketType mt) {
  switch (mt) {
  case MarketType::SPOT:
    return "stream.binance.com:9443";
  case MarketType::UM_PERP:
    return "fstream.binance.com:443";
  case MarketType::CM_PERP:
    return "dstream.binance.com:443";
  }
  return "";
}

struct PriceData {
  std::atomic<double> bid{0.0};
  std::atomic<double> ask{0.0};
  std::atomic<int64_t> timestamp{0};

  PriceData() = default;

  PriceData(const PriceData &other)
      : bid(other.bid.load(std::memory_order_relaxed)),
        ask(other.ask.load(std::memory_order_relaxed)),
        timestamp(other.timestamp.load(std::memory_order_relaxed)) {}

  PriceData(PriceData &&other) noexcept
      : bid(other.bid.load(std::memory_order_relaxed)),
        ask(other.ask.load(std::memory_order_relaxed)),
        timestamp(other.timestamp.load(std::memory_order_relaxed)) {}

  PriceData &operator=(const PriceData &other) {
    if (this != &other) {
      bid.store(other.bid.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
      ask.store(other.ask.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
      timestamp.store(other.timestamp.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }
    return *this;
  }

  PriceData &operator=(PriceData &&other) noexcept {
    if (this != &other) {
      bid.store(other.bid.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
      ask.store(other.ask.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
      timestamp.store(other.timestamp.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    }
    return *this;
  }

  void update(double bid_price, double ask_price, int64_t ts) {
    bid.store(bid_price, std::memory_order_relaxed);
    ask.store(ask_price, std::memory_order_relaxed);
    timestamp.store(ts, std::memory_order_relaxed);
  }

  struct Snapshot {
    double bid;
    double ask;
    int64_t timestamp;
  };

  Snapshot snapshot() const {
    return {bid.load(std::memory_order_relaxed),
            ask.load(std::memory_order_relaxed),
            timestamp.load(std::memory_order_relaxed)};
  }
};

struct PriceLevel {
  std::string price;
  std::string quantity;
};

struct BidComparator {
  bool operator()(const std::string &a, const std::string &b) const {
    return std::stod(a) > std::stod(b);
  }
};

struct AskComparator {
  bool operator()(const std::string &a, const std::string &b) const {
    return std::stod(a) < std::stod(b);
  }
};

class OrderBook {
public:
  OrderBook() : last_update_id_(0) {}

  void update_from_depth(const std::vector<PriceLevel> &bids,
                         const std::vector<PriceLevel> &asks) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto &level : bids) {
      if (level.quantity == "0" || level.quantity == "0.00000000") {
        bids_.erase(level.price);
      } else {
        bids_[level.price] = level.quantity;
      }
    }

    for (const auto &level : asks) {
      if (level.quantity == "0" || level.quantity == "0.00000000") {
        asks_.erase(level.price);
      } else {
        asks_[level.price] = level.quantity;
      }
    }

    ++last_update_id_;
  }

  struct BookSnapshot {
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;
    double mid_price;
    double spread;
  };

  BookSnapshot get_snapshot(size_t depth = 10) const {
    std::lock_guard<std::mutex> lock(mutex_);
    BookSnapshot snap;

    auto bid_it = bids_.begin();
    for (size_t i = 0; i < depth && bid_it != bids_.end(); ++i, ++bid_it) {
      snap.bids.push_back(
          {std::stod(bid_it->first), std::stod(bid_it->second)});
    }

    auto ask_it = asks_.begin();
    for (size_t i = 0; i < depth && ask_it != asks_.end(); ++i, ++ask_it) {
      snap.asks.push_back(
          {std::stod(ask_it->first), std::stod(ask_it->second)});
    }

    if (!bids_.empty() && !asks_.empty()) {
      double best_bid = std::stod(bids_.begin()->first);
      double best_ask = std::stod(asks_.begin()->first);
      snap.mid_price = (best_bid + best_ask) / 2.0;
      snap.spread = best_ask - best_bid;
    } else {
      snap.mid_price = 0.0;
      snap.spread = 0.0;
    }

    return snap;
  }

private:
  std::map<std::string, std::string, BidComparator> bids_;
  std::map<std::string, std::string, AskComparator> asks_;
  uint64_t last_update_id_;
  mutable std::mutex mutex_;
};

struct Leg {
  MarketType market;
  std::string symbol;
  PriceData price_data;
  std::shared_ptr<OrderBook> orderbook;
};

// LegRef is used to point to actual leg data.
struct LegRef {
  MarketType market;
  std::string symbol;
  double beta;
  std::shared_ptr<Leg> ptr;
};

struct Spread {
  std::string name;
  LegRef leg1, leg2;

  Spread() = default;

  struct SpreadSnapshot {
    double leg1_bid;
    double leg1_ask;
    int64_t leg1_ts;
    double leg2_bid;
    double leg2_ask;
    int64_t leg2_ts;
    double bid_spread;
    double ask_spread;
    double bid_ratio;
    double ask_ratio;
  };

  SpreadSnapshot calculate() const {
    auto l1 = leg1.ptr->price_data.snapshot();
    auto l2 = leg2.ptr->price_data.snapshot();

    SpreadSnapshot snap;
    snap.leg1_bid = l1.bid;
    snap.leg1_ask = l1.ask;
    snap.leg1_ts = l1.timestamp;
    snap.leg2_bid = l2.bid;
    snap.leg2_ask = l2.ask;
    snap.leg2_ts = l2.timestamp;
    snap.bid_spread = leg1.beta * l1.bid + leg2.beta * l2.bid;
    snap.ask_spread = leg1.beta * l1.ask + leg2.beta * l2.ask;
    snap.bid_ratio = (l1.bid != 0.0) ? snap.bid_spread / l1.bid : 0.0;
    snap.ask_ratio = (l1.ask != 0.0) ? snap.ask_spread / l1.ask : 0.0;
    return snap;
  }
};

struct BookTicker {
  double bid_price;
  double ask_price;
  int64_t timestamp;
};

struct OrderBookConfig {
  MarketType market;
  std::string symbol;
};
