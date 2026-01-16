#include "config.hpp"
#include "manager.hpp"
#include "types.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

using namespace ftxui;

// Thread-local buffer for formatting.
thread_local std::ostringstream g_format_buffer;
thread_local char g_number_buffer[32];

std::string format_double(double val, int precision = 5) {
  int len = std::snprintf(g_number_buffer, sizeof(g_number_buffer), "%.*f",
                          precision, val);
  return std::string(g_number_buffer, len);
}

std::string format_timestamp(int64_t us) {
  if (us == 0)
    return "0";

  auto ms = us / 1000;
  auto sec = ms / 1000;
  auto subsec_ms = ms % 1000;

  auto t = std::chrono::system_clock::from_time_t(sec);
  auto tt = std::chrono::system_clock::to_time_t(t);

  g_format_buffer.str("");
  g_format_buffer.clear();
  g_format_buffer << std::put_time(std::localtime(&tt), "%H:%M:%S");
  g_format_buffer << '.' << std::setfill('0') << std::setw(3) << subsec_ms;
  return g_format_buffer.str();
}

bool has_extension(const std::string &filename, const std::string &ext) {
  if (filename.length() < ext.length())
    return false;
  return filename.compare(filename.length() - ext.length(), ext.length(),
                          ext) == 0;
}

std::string get_base_filename(const std::string &path) {
  size_t last_slash = path.find_last_of("/\\");
  size_t last_dot = path.find_last_of('.');
  std::string base =
      (last_slash == std::string::npos) ? path : path.substr(last_slash + 1);
  if (last_dot != std::string::npos && last_dot > last_slash) {
    base = base.substr(
        0, last_dot - (last_slash == std::string::npos ? 0 : last_slash + 1));
  }
  return base;
}

void run_status_logging(const std::vector<Spread> &spreads_ref,
                        int refresh_rate_ms) {
  const size_t NUM_SPREADS = spreads_ref.size();

  std::cout << "\nStarting manager with " << NUM_SPREADS << " spreads...\n";
  std::cout << "Running in status logging mode (display_tui=false)\n";
  std::cout << "Press Ctrl+C to exit\n";

  std::atomic<bool> keep_running{true};

  int iteration = 0;
  while (keep_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(refresh_rate_ms));
    iteration++;
    // Do a quick status log every 10 seconds (not meant to be done real time).
    if (iteration % 100 == 0) {
      int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      int total_stale = 0;
      int total_zero = 0;
      int total_fresh = 0;
      constexpr int64_t STALE_THRESHOLD_US = 5'000'000;
      for (const auto &spread : spreads_ref) {
        auto snap = spread.calculate();
        // Check leg1.
        if (snap.leg1_ts == 0)
          ++total_zero;
        else if (now_us - snap.leg1_ts >= STALE_THRESHOLD_US)
          ++total_stale;
        else
          ++total_fresh;
        // Check leg2.
        if (snap.leg2_ts == 0)
          ++total_zero;
        else if (now_us - snap.leg2_ts >= STALE_THRESHOLD_US)
          ++total_stale;
        else
          ++total_fresh;
      }

      auto t = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
      std::cout << "\n[" << std::put_time(std::localtime(&t), "%H:%M:%S")
                << "] Status: Fresh=" << total_fresh
                << ", Stale=" << total_stale << ", NoData=" << total_zero
                << "\n";
    }
  }
}

void run_tui(const std::vector<Spread> &spreads_ref, int refresh_rate_ms) {
  const size_t NUM_SPREADS = spreads_ref.size();
  auto screen = ScreenInteractive::Fullscreen();
  // Preallocate vectors outside renderer.
  std::vector<Spread::SpreadSnapshot> snapshots;
  std::vector<std::vector<std::string>> rows;
  std::vector<double> bid_spreads, ask_spreads, bid_ratios, ask_ratios;
  snapshots.reserve(NUM_SPREADS);
  rows.reserve(NUM_SPREADS + 1);
  bid_spreads.reserve(NUM_SPREADS);
  ask_spreads.reserve(NUM_SPREADS);
  bid_ratios.reserve(NUM_SPREADS);
  ask_ratios.reserve(NUM_SPREADS);

  // Sorting state.
  enum class SortColumn {
    NONE,
    BID_SPREAD,
    ASK_SPREAD,
    BID_RATIO,
    ASK_RATIO,
    LEG1_BID,
    LEG1_ASK,
    LEG1_SYM,
    LEG2_SYM
  };
  SortColumn sort_by = SortColumn::NONE;
  bool sort_ascending = false;
  std::vector<size_t> sorted_indices;
  sorted_indices.reserve(NUM_SPREADS);

  // Renderer for the table.
  auto table_renderer = Renderer([&] {
    snapshots.clear();
    rows.clear();
    bid_spreads.clear();
    ask_spreads.clear();
    bid_ratios.clear();
    ask_ratios.clear();
    sorted_indices.clear();
    // Used for staleness check.
    int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (const auto &spread : spreads_ref)
      snapshots.push_back(spread.calculate());

    // Initialised sorted indices.
    for (size_t i = 0; i < NUM_SPREADS; ++i)
      sorted_indices.push_back(i);

    // Apply requested sorting.
    if (sort_by != SortColumn::NONE) {
      std::sort(sorted_indices.begin(), sorted_indices.end(),
                [&](size_t a, size_t b) {
                  const auto &snap_a = snapshots[a];
                  const auto &snap_b = snapshots[b];
                  const auto &spread_a = spreads_ref[a];
                  const auto &spread_b = spreads_ref[b];
                  bool less = false;
                  switch (sort_by) {
                  case SortColumn::BID_SPREAD:
                    less = snap_a.bid_spread < snap_b.bid_spread;
                    break;
                  case SortColumn::ASK_SPREAD:
                    less = snap_a.ask_spread < snap_b.ask_spread;
                    break;
                  case SortColumn::BID_RATIO:
                    less = snap_a.bid_ratio < snap_b.bid_ratio;
                    break;
                  case SortColumn::ASK_RATIO:
                    less = snap_a.ask_ratio < snap_b.ask_ratio;
                    break;
                  case SortColumn::LEG1_BID:
                    less = snap_a.leg1_bid < snap_b.leg1_bid;
                    break;
                  case SortColumn::LEG1_ASK:
                    less = snap_a.leg1_ask < snap_b.leg1_ask;
                    break;
                  case SortColumn::LEG1_SYM:
                    less = spread_a.leg1.symbol < spread_b.leg1.symbol;
                    break;
                  case SortColumn::LEG2_SYM:
                    less = spread_a.leg2.symbol < spread_b.leg2.symbol;
                    break;
                  default:
                    less = false;
                  }
                  return sort_ascending ? less : !less;
                });
    }
    // Header.
    rows.push_back({"#", "Leg1_Mkt", "Leg1_Sym", "Î²1", "Leg2_Mkt", "Leg2_Sym",
                    "Î²2", "L1_Bid", "L1_Ask", "L2_Bid", "L2_Ask", "Bid_Spread",
                    "Ask_Spread", "Bid_Ratio", "Ask_Ratio", "L1_Time",
                    "L2_Time"});

    // Data.
    for (size_t i = 0; i < NUM_SPREADS; ++i) {
      size_t idx = sorted_indices[i];
      const auto &snap = snapshots[idx];
      const auto &spread = spreads_ref[idx];
      bid_spreads.push_back(snap.bid_spread);
      ask_spreads.push_back(snap.ask_spread);
      bid_ratios.push_back(snap.bid_ratio);
      ask_ratios.push_back(snap.ask_ratio);
      rows.push_back(
          {std::to_string(i + 1),
           std::string(market_type_to_string(spread.leg1.market)),
           spread.leg1.symbol, format_double(spread.leg1.beta, 1),
           std::string(market_type_to_string(spread.leg2.market)),
           spread.leg2.symbol, format_double(spread.leg2.beta, 1),
           format_double(snap.leg1_bid, 5), format_double(snap.leg1_ask, 5),
           format_double(snap.leg2_bid, 5), format_double(snap.leg2_ask, 5),
           format_double(snap.bid_spread, 5), format_double(snap.ask_spread, 5),
           format_double(snap.bid_ratio, 5), format_double(snap.ask_ratio, 5),
           format_timestamp(snap.leg1_ts), format_timestamp(snap.leg2_ts)});
    }
    auto table = Table(rows);
    table.SelectAll().Border(LIGHT);
    table.SelectRow(0).Decorate(bold);
    table.SelectRows(1, -1).SeparatorVertical(LIGHT);
    table.SelectRows(0, 0).BorderBottom(LIGHT);
    for (size_t i = 0; i <= NUM_SPREADS; ++i)
      table.SelectRow(i).SeparatorVertical(LIGHT);

    // Colour bid_spread (11) and ask_spread (12).
    // Colour bid_ratio (13) and ask_ratio (14).
    // Colour stale (>= 5s) timestamps, L1_Time (15), L2_Time (16).
    constexpr int64_t STALE_THRESHOLD_US = 5'000'000;
    for (size_t i = 0; i < NUM_SPREADS; ++i) {
      table.SelectCell(11, i + 1).Decorate(
          bid_spreads[i] >= 0 ? color(Color::Green) : color(Color::Red));
      table.SelectCell(12, i + 1).Decorate(
          ask_spreads[i] >= 0 ? color(Color::Green) : color(Color::Red));
      table.SelectCell(13, i + 1).Decorate(
          bid_ratios[i] >= 0 ? color(Color::Green) : color(Color::Red));
      table.SelectCell(14, i + 1).Decorate(
          ask_ratios[i] >= 0 ? color(Color::Green) : color(Color::Red));
      const auto &snap = snapshots[i];
      if (now_us - snap.leg1_ts >= STALE_THRESHOLD_US)
        table.SelectCell(15, i + 1).Decorate(color(Color::Orange1));
      if (now_us - snap.leg2_ts >= STALE_THRESHOLD_US)
        table.SelectCell(16, i + 1).Decorate(color(Color::Orange1));
    }
    return table.Render();
  });

  // Create sorting buttons.
  auto btn_bid_spread = Button("Sort Bid Spread", [&] {
    if (sort_by == SortColumn::BID_SPREAD) {
      sort_ascending = !sort_ascending;
    } else {
      sort_by = SortColumn::BID_SPREAD;
      sort_ascending = false;
    }
  });

  auto btn_ask_spread = Button("Sort Ask Spread", [&] {
    if (sort_by == SortColumn::ASK_SPREAD) {
      sort_ascending = !sort_ascending;
    } else {
      sort_by = SortColumn::ASK_SPREAD;
      sort_ascending = false;
    }
  });

  auto btn_bid_ratio = Button("Sort Bid Ratio", [&] {
    if (sort_by == SortColumn::BID_RATIO) {
      sort_ascending = !sort_ascending;
    } else {
      sort_by = SortColumn::BID_RATIO;
      sort_ascending = false;
    }
  });

  auto btn_ask_ratio = Button("Sort Ask Ratio", [&] {
    if (sort_by == SortColumn::ASK_RATIO) {
      sort_ascending = !sort_ascending;
    } else {
      sort_by = SortColumn::ASK_RATIO;
      sort_ascending = false;
    }
  });

  auto btn_leg1_sym = Button("Sort Leg1 Sym", [&] {
    if (sort_by == SortColumn::LEG1_SYM) {
      sort_ascending = !sort_ascending;
    } else {
      sort_by = SortColumn::LEG1_SYM;
      sort_ascending = true; // Alphabetical sorting starts ascending
    }
  });

  auto btn_leg2_sym = Button("Sort Leg2 Sym", [&] {
    if (sort_by == SortColumn::LEG2_SYM) {
      sort_ascending = !sort_ascending;
    } else {
      sort_by = SortColumn::LEG2_SYM;
      sort_ascending = true; // Alphabetical sorting starts ascending
    }
  });

  auto btn_reset = Button("Reset Sort", [&] {
    sort_by = SortColumn::NONE;
    sort_ascending = false;
  });

  // Layout buttons horizontally.
  auto buttons = Container::Horizontal({btn_bid_spread, btn_ask_spread,
                                        btn_bid_ratio, btn_ask_ratio,
                                        btn_leg1_sym, btn_leg2_sym, btn_reset});

  // Add sort indicator text
  auto sort_indicator = Renderer([&] {
    std::string sort_text = "Sort: ";
    switch (sort_by) {
    case SortColumn::BID_SPREAD:
      sort_text += "Bid Spread";
      break;
    case SortColumn::ASK_SPREAD:
      sort_text += "Ask Spread";
      break;
    case SortColumn::BID_RATIO:
      sort_text += "Bid Ratio";
      break;
    case SortColumn::ASK_RATIO:
      sort_text += "Ask Ratio";
      break;
    case SortColumn::LEG1_SYM:
      sort_text += "Leg1 Symbol";
      break;
    case SortColumn::LEG2_SYM:
      sort_text += "Leg2 Symbol";
      break;
    default:
      sort_text += "None";
      break;
    }
    if (sort_by != SortColumn::NONE) {
      sort_text += sort_ascending ? " (Asc)" : " (Desc)";
    }
    return text(sort_text) | bold;
  });

  // Scrolling state (0.0 to 1.0).
  float scroll_y = 0.0f;

  // Wrap table with scrollable frame.
  auto scrollable_table = Renderer(table_renderer, [&] {
    return table_renderer->Render() | focusPositionRelative(0.0f, scroll_y) |
           frame | flex;
  });

  // Combine controls and table vertically.
  auto component = Container::Vertical({buttons, scrollable_table});

  component = Renderer(component, [&] {
    return vbox({hbox({
                     text("Controls: ") | bold,
                     buttons->Render(),
                 }),
                 sort_indicator->Render(), separator(),
                 scrollable_table->Render() | flex});
  });

  component = CatchEvent(component, [&](Event event) {
    // 1% of content per scroll action.
    const float increment = 0.01f;
    // Row by row.
    if (event == Event::ArrowUp) {
      scroll_y = std::max(0.0f, scroll_y - increment);
      return true;
    }
    if (event == Event::ArrowDown) {
      scroll_y = std::min(1.0f, scroll_y + increment);
      return true;
    }
    // Faster scroll.
    if (event == Event::PageUp) {
      scroll_y = std::max(0.0f, scroll_y - 0.1f);
      return true;
    }
    if (event == Event::PageDown) {
      scroll_y = std::min(1.0f, scroll_y + 0.1f);
      return true;
    }
    // Mouse wheel.
    if (event.is_mouse()) {
      if (event.mouse().button == Mouse::WheelUp) {
        scroll_y = std::max(0.0f, scroll_y - increment);
        return true;
      }
      if (event.mouse().button == Mouse::WheelDown) {
        scroll_y = std::min(1.0f, scroll_y + increment);
        return true;
      }
    }
    return false;
  });

  // Auto-refresh.
  std::atomic<bool> refresh_ui = true;
  std::thread refresh_thread([&, refresh_rate_ms] {
    while (refresh_ui) {
      screen.PostEvent(Event::Custom);
      std::this_thread::sleep_for(std::chrono::milliseconds(refresh_rate_ms));
    }
  });
  screen.Loop(component);
  // Cleanup.
  refresh_ui = false;
  refresh_thread.join();
}

int main(int argc, char **argv) {
  std::string config_path = argc > 1 ? argv[1] : "spreads.yaml";
  try {
    Config config;
    config = load_config(config_path);
    std::sort(config.spreads.begin(), config.spreads.end(),
              [](const Spread &a, const Spread &b) {
                return (a.leg1.market == b.leg1.market)
                           ? a.leg1.symbol < b.leg1.symbol
                           : market_type_to_string(a.leg1.market) <
                                 market_type_to_string(b.leg1.market);
              });
    SpreadManager manager(std::move(config.spreads));
    const auto &spreads_ref = manager.get_spreads();
    manager.start();
    // Choose whether or not to display TUI.
    if (config.display_tui)
      run_tui(spreads_ref, config.refresh_rate_ms);
    else
      run_status_logging(spreads_ref, config.refresh_rate_ms);
    manager.stop();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
// #include "httpclient.hpp"
// #include "orderbook.hpp"
// #include "wsclient.hpp"
// #include <chrono>
// #include <iostream>
// #include <mutex>
// #include <queue>
// #include <string>
// #include <thread>
//
// class OrderBookManager {
// public:
//   OrderBookManager(const std::string &symbol)
//       : symbol_(symbol), orderBook_(), httpClient_(), wsClient_(symbol),
//         snapshotReceived_(false) {}
//
//   void start() {
//     // Set up WebSocket callback.
//     wsClient_.setMessageCallback(
//         [this](const DepthUpdate &update) { this->onDepthUpdate(update); });
//
//     // Start WebSocket connection.
//     wsClient_.connect();
//
//     // Buffer initial messages for a longer period.
//     std::cout << "Buffering WebSocket messages." << std::endl;
//     std::this_thread::sleep_for(std::chrono::seconds(2));
//
//     // Fetch snapshot.
//     bool success = false;
//     int attempts = 0;
//     const int maxAttempts = 3;
//
//     while (!success && attempts < maxAttempts) {
//       std::cout << "Fetching depth snapshot (attempt " << (attempts + 1) <<
//       ")."
//                 << std::endl;
//
//       auto snapshot = httpClient_.getSnapshot(symbol_);
//
//       if (!snapshot.success) {
//         attempts++;
//         std::this_thread::sleep_for(std::chrono::seconds(1));
//         continue;
//       }
//
//       // Check if need to re-fetch.
//       std::lock_guard<std::mutex> lock(bufferMutex_);
//       if (!updateBuffer_.empty()) {
//         uint64_t firstBufferedU = updateBuffer_.front().firstUpdateId;
//
//         if (snapshot.lastUpdateId < firstBufferedU) {
//           std::cout << "Refetching snapshot." << std::endl;
//           attempts++;
//           std::this_thread::sleep_for(std::chrono::seconds(1));
//           continue;
//         }
//       }
//
//       // Apply snapshot.
//       orderBook_.setSnapshot(snapshot.bids, snapshot.asks,
//                              snapshot.lastUpdateId);
//       snapshotReceived_ = true;
//
//       // Process buffered updates.
//       processBufferedUpdates(snapshot.lastUpdateId);
//
//       success = true;
//     }
//
//     if (!success) {
//       std::cerr << "Failed to initialise order book after " << maxAttempts
//                 << " attempts." << std::endl;
//       return;
//     }
//
//     std::cout << "Order book initialised." << std::endl;
//     orderBook_.display();
//
//     // Loop to periodically redisplay order book.
//     while (true) {
//       std::this_thread::sleep_for(std::chrono::seconds(60));
//     }
//   }
//
// private:
//   std::string symbol_;
//   OrderBook orderBook_;
//   HttpClient httpClient_;
//   WebSocketClient wsClient_;
//
//   std::queue<DepthUpdate> updateBuffer_;
//   std::mutex bufferMutex_;
//   bool snapshotReceived_;
//
//   void onDepthUpdate(const DepthUpdate &update) {
//     std::lock_guard<std::mutex> lock(bufferMutex_);
//
//     if (!snapshotReceived_) {
//       updateBuffer_.push(update);
//     } else {
//       // Apply update after snapshot and display immediately.
//       if (orderBook_.update(update)) {
//         orderBook_.display();
//       } else {
//         std::cerr << "Failed to apply update [" << update.firstUpdateId << ",
//         "
//                   << update.finalUpdateId << "]" << std::endl;
//       }
//     }
//   }
//
//   void processBufferedUpdates(uint64_t snapshotLastUpdateId) {
//     std::cout << "Processing buffered updates." << std::endl;
//     std::cout << "Snapshot lastUpdateId: " << snapshotLastUpdateId <<
//     std::endl; std::cout << "Buffer count: " << updateBuffer_.size() <<
//     std::endl;
//
//     if (updateBuffer_.empty()) {
//       std::cout << "No buffered updates, might not be "
//                    "receiving messages."
//                 << std::endl;
//       return;
//     }
//
//     int discarded = 0;
//     int applied = 0;
//
//     while (!updateBuffer_.empty()) {
//       DepthUpdate update = updateBuffer_.front();
//       updateBuffer_.pop();
//
//       if (discarded == 0 && applied == 0) {
//         std::cout << "First buffered update: U=" << update.firstUpdateId
//                   << ", u=" << update.finalUpdateId << std::endl;
//       }
//
//       // Discard events where u <= lastUpdateId.
//       if (update.finalUpdateId <= snapshotLastUpdateId) {
//         discarded++;
//         continue;
//       }
//
//       // First valid event should have lastUpdateId within [U, u].
//       if (applied == 0) {
//         if (update.firstUpdateId > snapshotLastUpdateId + 1) {
//           std::cerr << "Gap between snapshot and first valid update."
//                     << std::endl;
//           std::cerr << "Snapshot lastUpdateId: " << snapshotLastUpdateId
//                     << std::endl;
//           std::cerr << "First update U: " << update.firstUpdateId <<
//           std::endl;
//         }
//       }
//
//       if (orderBook_.update(update)) {
//         applied++;
//       }
//     }
//
//     std::cout << "Discarded " << discarded << " old updates, applied "
//               << applied << " updates." << std::endl;
//   }
// };
//
// int main() {
//   std::string symbol;
//
//   std::cout << "Enter Binance symbol (e.g., BTCUSDT, ETHUSDT): ";
//   std::getline(std::cin, symbol);
//
//   if (symbol.empty()) {
//     std::cerr << "Invalid symbol" << std::endl;
//     return 1;
//   }
//
//   // Convert to uppercase for API.
//   std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
//
//   std::cout << "Starting order book for " << symbol << "." << std::endl;
//   std::cout << "Press Ctrl-C to exit." << std::endl;
//
//   try {
//     OrderBookManager manager(symbol);
//     manager.start();
//   } catch (const std::exception &e) {
//     std::cerr << "Fatal error: " << e.what() << std::endl;
//     return 1;
//   }
//
//   return 0;
// }
