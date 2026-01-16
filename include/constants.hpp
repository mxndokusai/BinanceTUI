#pragma once
#include "types.hpp"
#include <cstddef>

constexpr size_t NUM_MARKETS = 3;

constexpr size_t MARKET_SPOT = 0;
constexpr size_t MARKET_UM_PERP = 1;
constexpr size_t MARKET_CM_PERP = 2;

inline constexpr size_t market_to_index(MarketType mt) {
  switch (mt) {
  case MarketType::SPOT:
    return MARKET_SPOT;
  case MarketType::UM_PERP:
    return MARKET_UM_PERP;
  case MarketType::CM_PERP:
    return MARKET_CM_PERP;
  }
  return 0;
}
