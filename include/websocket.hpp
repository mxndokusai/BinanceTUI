#pragma once
#include "types.hpp"
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/version.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

enum class StreamType { BOOK_TICKER, DEPTH };

class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
  using BookTickerCallback =
      std::function<void(const std::string &, const BookTicker &)>;
  using DepthCallback =
      std::function<void(const std::string &, const std::vector<PriceLevel> &,
                         const std::vector<PriceLevel> &)>;
  net::io_context &ioc_;
  ssl::context &ssl_ctx_;
  tcp::resolver resolver_;
  std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
  beast::flat_buffer buffer_;
  std::string host_, port_;
  std::vector<std::string> symbols_;
  StreamType stream_type_;
  BookTickerCallback book_ticker_callback_;
  DepthCallback depth_callback_;
  std::atomic<bool> running_{false};
  std::size_t market_id_;
  std::shared_ptr<boost::asio::steady_timer> reconnect_timer_;

public:
  WebSocketClient(net::io_context &ioc, ssl::context &ctx,
                  const std::string &host, const std::string &port,
                  const std::vector<std::string> &symbols,
                  StreamType stream_type, BookTickerCallback book_ticker_cb,
                  DepthCallback depth_cb, const std::size_t market_id = -1)
      : ioc_(ioc), ssl_ctx_(ctx), resolver_(net::make_strand(ioc)), host_(host),
        port_(port), symbols_(symbols), stream_type_(stream_type),
        book_ticker_callback_(std::move(book_ticker_cb)),
        depth_callback_(std::move(depth_cb)), market_id_(market_id),
        reconnect_timer_(std::make_shared<boost::asio::steady_timer>(ioc)) {
    ws_ = std::make_unique<
        websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
        net::make_strand(ioc), ssl_ctx_);
  }

  void run() {
    running_ = true;
    ws_ = std::make_unique<
        websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
        net::make_strand(ioc_), ssl_ctx_);
    buffer_.clear();
    resolver_.async_resolve(
        host_, port_,
        beast::bind_front_handler(&WebSocketClient::on_resolve,
                                  shared_from_this()));
  }

  void stop() {
    running_ = false;
    if (reconnect_timer_) {
#if BOOST_VERSION >= 109000
      reconnect_timer_->cancel();
#else
      boost::system::error_code ec;
      reconnect_timer_->cancel(ec);
#endif
    }
  }

private:
  void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec)
      return fail(ec, "resolve");
    if (!running_)
      return;
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*ws_).async_connect(
        results, beast::bind_front_handler(&WebSocketClient::on_connect,
                                           shared_from_this()));
  }

  void on_connect(beast::error_code ec,
                  tcp::resolver::results_type::endpoint_type) {
    if (ec)
      return fail(ec, "connect");
    if (!running_)
      return;

    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                  host_.c_str())) {
      ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                             net::error::get_ssl_category());
      return fail(ec, "ssl_set_hostname");
    }
    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&WebSocketClient::on_ssl_handshake,
                                  shared_from_this()));
  }

  void on_ssl_handshake(beast::error_code ec) {
    if (ec)
      return fail(ec, "ssl_handshake");
    if (!running_)
      return;

    beast::get_lowest_layer(*ws_).expires_never();
    ws_->set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::client));
    // Build path based on stream type.
    std::string path = "/stream?streams=";
    const char *stream_suffix =
        (stream_type_ == StreamType::BOOK_TICKER) ? "@bookTicker" : "@depth";
    for (size_t i = 0; i < symbols_.size(); ++i) {
      if (i > 0)
        path += "/";
      path += symbols_[i] + stream_suffix;
    }
    ws_->async_handshake(
        host_, path,
        beast::bind_front_handler(&WebSocketClient::on_handshake,
                                  shared_from_this()));
  }

  void on_handshake(beast::error_code ec) {
    if (ec)
      return fail(ec, "handshake");
    if (!running_)
      return;
    std::cout << "[" << market_id_ << "] Connected with " << symbols_.size()
              << " symbols ("
              << (stream_type_ == StreamType::BOOK_TICKER ? "bookTicker"
                                                          : "depth")
              << ")\n";
    do_read();
  }

  void do_read() {
    if (!running_)
      return;
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&WebSocketClient::on_read,
                                              shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    if (ec) {
      if (!running_)
        return;
      if (ec == net::error::eof || ec == websocket::error::closed ||
          ec == net::error::connection_reset) {
        std::cerr << "[" << market_id_ << "] Connection lost (" << ec.message()
                  << "), reconnecting in 1s...\n";
        buffer_.clear();
        reconnect_timer_->expires_after(std::chrono::seconds(1));
        reconnect_timer_->async_wait(
            [self = shared_from_this()](beast::error_code ec) {
              if (!ec && self->running_) {
                std::cerr << "[" << self->market_id_ << "] Reconnecting...\n";
                self->run();
              }
            });
        return;
      }
      std::cerr << "[" << market_id_ << "] Read error: " << ec.message()
                << "\n";
      return;
    }
    try {
      auto data = beast::buffers_to_string(buffer_.data());
      buffer_.consume(buffer_.size());
      nlohmann::json json;
      try {
        json = nlohmann::json::parse(data);
      } catch (const nlohmann::json::parse_error &e) {
        std::cerr << "[" << market_id_ << "] JSON parse error: " << e.what()
                  << "\n";
        do_read();
        return;
      }
      if (!json.contains("stream") || !json.contains("data")) {
        do_read();
        return;
      }
      std::string stream = json["stream"].get<std::string>();
      size_t at_pos = stream.find('@');
      std::string symbol =
          (at_pos != std::string::npos) ? stream.substr(0, at_pos) : "";
      if (symbol.empty()) {
        do_read();
        return;
      }
      auto data_obj = json["data"];
      if (stream_type_ == StreamType::BOOK_TICKER) {
        // Parse book ticker.
        if (!data_obj.contains("b") || !data_obj.contains("a")) {
          std::cerr << "[" << market_id_ << "] Missing bid/ask in message\n";
          do_read();
          return;
        }

        BookTicker ticker;
        ticker.bid_price = std::stod(data_obj["b"].get<std::string>());
        ticker.ask_price = std::stod(data_obj["a"].get<std::string>());
        auto now = std::chrono::system_clock::now();
        ticker.timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count();

        if (book_ticker_callback_)
          book_ticker_callback_(symbol, ticker);

      } else if (stream_type_ == StreamType::DEPTH) {
        // Parse depth update.
        if (!data_obj.contains("b") || !data_obj.contains("a")) {
          std::cerr << "[" << market_id_
                    << "] Missing bids/asks in depth message\n";
          do_read();
          return;
        }

        std::vector<PriceLevel> bids, asks;
        // Parse bids.
        for (const auto &bid : data_obj["b"]) {
          if (bid.is_array() && bid.size() >= 2) {
            PriceLevel level;
            level.price = bid[0].get<std::string>();
            level.quantity = bid[1].get<std::string>();
            bids.push_back(level);
          }
        }
        // Parse asks.
        for (const auto &ask : data_obj["a"]) {
          if (ask.is_array() && ask.size() >= 2) {
            PriceLevel level;
            level.price = ask[0].get<std::string>();
            level.quantity = ask[1].get<std::string>();
            asks.push_back(level);
          }
        }
        if (depth_callback_)
          depth_callback_(symbol, bids, asks);
      }
    } catch (const std::exception &e) {
      std::cerr << "[" << market_id_ << "] Parse error: " << e.what() << "\n";
    }
    buffer_.consume(buffer_.size());
    do_read();
  }

  void fail(beast::error_code ec, const char *what) {
    if (ec == net::error::operation_aborted && !running_)
      return;
    std::cerr << "[" << market_id_ << "] " << what
              << " failed: " << ec.message() << "\n";
  }
};
