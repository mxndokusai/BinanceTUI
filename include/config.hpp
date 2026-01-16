#pragma once
#include "types.hpp"
#include <stdexcept>
#include <vector>
#include <yaml-cpp/yaml.h>

inline MarketType parse_market_type(const std::string &s) {
  if (s == "SPOT")
    return MarketType::SPOT;
  if (s == "PERP" || s == "UM_PERP" || s == "UM_FUTURES")
    return MarketType::UM_PERP;
  if (s == "CPERP" || s == "CM_PERP" || s == "CM_FUTURES")
    return MarketType::CM_PERP;
  throw std::runtime_error("Unknown market type: " + s);
}

struct Config {
  std::vector<Spread> spreads;
  std::vector<OrderBookConfig> orderbooks;
  int refresh_rate_ms = 100; // Defaults to 100 ms.
  bool display_tui = true;
  std::string view_mode = "spreads";
};

inline Config load_config(const std::string &config_path) {
  YAML::Node root = YAML::LoadFile(config_path);
  Config config;
  config.refresh_rate_ms = root["refresh_rate_ms"].as<int>();
  if (config.refresh_rate_ms <= 0 || config.refresh_rate_ms > 10000) {
    throw std::runtime_error("refresh_rate_ms must be between 1 and 10000");
  }
  if (root["display_tui"]) {
    config.display_tui = root["display_tui"].as<bool>();
  }
  if (root["view_mode"]) {
    config.view_mode = root["view_mode"].as<std::string>();
  }
  if (root["spreads"]) {
    for (const auto &item : root["spreads"]) {
      Spread spread;
      spread.name = item["name"].as<std::string>();
      spread.leg1.market =
          parse_market_type(item["leg1"]["market"].as<std::string>());
      spread.leg1.symbol = item["leg1"]["symbol"].as<std::string>();
      spread.leg1.beta = item["leg1"]["beta"].as<double>();
      spread.leg2.market =
          parse_market_type(item["leg2"]["market"].as<std::string>());
      spread.leg2.symbol = item["leg2"]["symbol"].as<std::string>();
      spread.leg2.beta = item["leg2"]["beta"].as<double>();
      config.spreads.push_back(std::move(spread));
    }
  }
  if (root["orderbooks"]) {
    for (const auto &item : root["orderbooks"]) {
      OrderBookConfig ob;
      ob.market = parse_market_type(item["market"].as<std::string>());
      ob.symbol = item["symbol"].as<std::string>();
      config.orderbooks.push_back(std::move(ob));
    }
  }
  return config;
}
