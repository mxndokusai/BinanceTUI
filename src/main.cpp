#include "config.hpp"
#include "manager.hpp"
#include "types.hpp"
#include <algorithm>
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <iomanip>
#include <iostream>
#include <spdlog/spdlog.h>
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

void run_spread_tui(const std::vector<Spread> &spreads_ref,
                    int refresh_rate_ms) {
  const size_t NUM_SPREADS = spreads_ref.size();
  auto screen = ScreenInteractive::Fullscreen();

  std::vector<Spread::SpreadSnapshot> snapshots;
  std::vector<std::vector<std::string>> rows;
  std::vector<double> bid_spreads, ask_spreads, bid_ratios, ask_ratios;
  snapshots.reserve(NUM_SPREADS);
  rows.reserve(NUM_SPREADS + 1);
  bid_spreads.reserve(NUM_SPREADS);
  ask_spreads.reserve(NUM_SPREADS);
  bid_ratios.reserve(NUM_SPREADS);
  ask_ratios.reserve(NUM_SPREADS);

  enum class SortColumn {
    NONE,
    BID_SPREAD,
    ASK_SPREAD,
    BID_RATIO,
    ASK_RATIO,
    LEG1_SYM,
    LEG2_SYM
  };
  SortColumn sort_by = SortColumn::NONE;
  bool sort_ascending = false;
  std::vector<size_t> sorted_indices;
  sorted_indices.reserve(NUM_SPREADS);

  auto table_renderer = Renderer([&] {
    snapshots.clear();
    rows.clear();
    bid_spreads.clear();
    ask_spreads.clear();
    bid_ratios.clear();
    ask_ratios.clear();
    sorted_indices.clear();

    int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    for (const auto &spread : spreads_ref)
      snapshots.push_back(spread.calculate());

    for (size_t i = 0; i < NUM_SPREADS; ++i)
      sorted_indices.push_back(i);

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

    rows.push_back({"#", "Leg1_Mkt", "Leg1_Sym", "β1", "Leg2_Mkt", "Leg2_Sym",
                    "β2", "L1_Bid", "L1_Ask", "L2_Bid", "L2_Ask", "Bid_Spread",
                    "Ask_Spread", "Bid_Ratio", "Ask_Ratio", "L1_Time",
                    "L2_Time"});

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

  auto btn_bid_spread = Button("Sort Bid Spread", [&] {
    if (sort_by == SortColumn::BID_SPREAD)
      sort_ascending = !sort_ascending;
    else {
      sort_by = SortColumn::BID_SPREAD;
      sort_ascending = false;
    }
  });

  auto btn_ask_spread = Button("Sort Ask Spread", [&] {
    if (sort_by == SortColumn::ASK_SPREAD)
      sort_ascending = !sort_ascending;
    else {
      sort_by = SortColumn::ASK_SPREAD;
      sort_ascending = false;
    }
  });

  auto btn_reset = Button("Reset Sort", [&] {
    sort_by = SortColumn::NONE;
    sort_ascending = false;
  });

  auto buttons =
      Container::Horizontal({btn_bid_spread, btn_ask_spread, btn_reset});

  float scroll_y = 0.0f;
  auto scrollable_table = Renderer(table_renderer, [&] {
    return table_renderer->Render() | focusPositionRelative(0.0f, scroll_y) |
           frame | flex;
  });

  auto component = Container::Vertical({buttons, scrollable_table});
  component = Renderer(component, [&] {
    return vbox(
        {hbox({text("Spreads View") | bold, separator(), buttons->Render()}),
         separator(), scrollable_table->Render() | flex});
  });

  component = CatchEvent(component, [&](Event event) {
    const float increment = 0.01f;
    if (event == Event::ArrowUp) {
      scroll_y = std::max(0.0f, scroll_y - increment);
      return true;
    }
    if (event == Event::ArrowDown) {
      scroll_y = std::min(1.0f, scroll_y + increment);
      return true;
    }
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

  std::atomic<bool> refresh_ui = true;
  std::thread refresh_thread([&, refresh_rate_ms] {
    while (refresh_ui) {
      screen.PostEvent(Event::Custom);
      std::this_thread::sleep_for(std::chrono::milliseconds(refresh_rate_ms));
    }
  });

  screen.Loop(component);
  refresh_ui = false;
  refresh_thread.join();
}

void run_orderbook_tui(DataManager &manager,
                       const std::vector<OrderBookConfig> &configs,
                       int refresh_rate_ms) {
  auto screen = ScreenInteractive::Fullscreen();

  int selected_book = 0;

  auto renderer = Renderer([&] {
    if (configs.empty()) {
      return text("No orderbooks configured") | center;
    }

    const auto &config = configs[selected_book];
    std::string key =
        std::string(market_type_to_string(config.market)) + "_" + config.symbol;
    auto orderbook = manager.get_orderbook(key);

    if (!orderbook) {
      return text("Orderbook not available") | center;
    }

    auto snapshot = orderbook->get_snapshot(10);

    std::vector<std::vector<std::string>> rows;
    rows.push_back({"", "Price", "Quantity"});
    // Show asks in reverse.
    for (auto it = snapshot.asks.rbegin(); it != snapshot.asks.rend(); ++it)
      rows.push_back(
          {"ASK", format_double(it->first, 2), format_double(it->second, 6)});
    // Mid price row.
    rows.push_back({"MID", format_double(snapshot.mid_price, 2),
                    "Spread: " + format_double(snapshot.spread, 2)});
    // Show bids in order.
    for (const auto &bid : snapshot.bids)
      rows.push_back(
          {"BID", format_double(bid.first, 2), format_double(bid.second, 6)});

    auto table = Table(rows);
    table.SelectAll().Border(LIGHT);
    table.SelectRow(0).Decorate(bold);
    table.SelectRows(1, -1).SeparatorVertical(LIGHT);
    table.SelectRows(0, 0).BorderBottom(HEAVY);
    // Colour asks red.
    for (size_t i = 1; i <= snapshot.asks.size(); ++i)
      table.SelectRow(i).Decorate(color(Color::Red));
    // Colour mid row.
    size_t mid_row = snapshot.asks.size() + 1;
    table.SelectRow(mid_row).Decorate(bold | color(Color::Yellow));
    table.SelectRow(mid_row).BorderTop(HEAVY);
    table.SelectRow(mid_row).BorderBottom(HEAVY);
    // Colour bids green.
    for (size_t i = mid_row + 1; i < rows.size(); ++i)
      table.SelectRow(i).Decorate(color(Color::Green));
    std::string title = config.symbol + " (" +
                        std::string(market_type_to_string(config.market)) + ")";
    return vbox({text(title) | bold | center, separator(),
                 table.Render() | center, separator(),
                 text("← → to switch orderbooks | q to quit") | dim | center});
  });

  auto component = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.ExitLoopClosure()();
      return true;
    }
    if (event == Event::ArrowLeft && selected_book > 0) {
      selected_book--;
      return true;
    }
    if (event == Event::ArrowRight && selected_book < (int)configs.size() - 1) {
      selected_book++;
      return true;
    }
    return false;
  });

  std::atomic<bool> refresh_ui = true;
  std::thread refresh_thread([&, refresh_rate_ms] {
    while (refresh_ui) {
      screen.PostEvent(Event::Custom);
      std::this_thread::sleep_for(std::chrono::milliseconds(refresh_rate_ms));
    }
  });

  screen.Loop(component);
  refresh_ui = false;
  refresh_thread.join();
}

int main(int argc, char **argv) {
  std::string config_path = argc > 1 ? argv[1] : "config.yaml";

  try {
    Config config = load_config(config_path);

    spdlog::info("Loaded {} spreads and {} orderbooks", config.spreads.size(),
                 config.orderbooks.size());

    // Sort spreads.
    std::sort(config.spreads.begin(), config.spreads.end(),
              [](const Spread &a, const Spread &b) {
                return (a.leg1.market == b.leg1.market)
                           ? a.leg1.symbol < b.leg1.symbol
                           : market_type_to_string(a.leg1.market) <
                                 market_type_to_string(b.leg1.market);
              });

    DataManager manager(std::move(config.spreads),
                        std::move(config.orderbooks));
    manager.start();

    std::this_thread::sleep_for(std::chrono::seconds(2));

    if (!config.display_tui) {
      spdlog::info("Running in non-TUI mode, press Ctrl+C to exit");
      while (true)
        std::this_thread::sleep_for(std::chrono::seconds(60));
    } else {
      if (config.view_mode == "orderbook" &&
          !manager.get_orderbook_configs().empty()) {
        run_orderbook_tui(manager, manager.get_orderbook_configs(),
                          config.refresh_rate_ms);
      } else if (!manager.get_spreads().empty()) {
        run_spread_tui(manager.get_spreads(), config.refresh_rate_ms);
      } else {
        std::cerr << "No spreads or orderbooks configured!\n";
        return 1;
      }
    }
    manager.stop();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
