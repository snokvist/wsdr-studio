// SPDX-License-Identifier: GPL-2.0-only
#include "server.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <deque>
#include <filesystem>

namespace studio {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxQueuedBinary = 32;
constexpr size_t kMaxQueuedText = 1024; // a client this far behind is dropped

bool host_ok(const ServerOptions &o, beast::string_view host) {
  const std::string p = ":" + std::to_string(o.port);
  return host == "127.0.0.1" + p || host == "localhost" + p;
}

bool origin_ok(const ServerOptions &o, beast::string_view origin) {
  if (origin.empty())
    return true; // non-browser client (browsers always send Origin on WebSocket requests)
  const std::string p = ":" + std::to_string(o.port);
  if (origin == "http://127.0.0.1" + p || origin == "http://localhost" + p)
    return true;
  for (const auto &e : o.extra_origins)
    if (origin == e)
      return true;
  return false;
}

bool same(const std::string &a, const std::string &b) { // constant time for equal lengths
  if (a.size() != b.size() || a.empty())
    return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); i++)
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

// Value of `key` in a query string "a=1&b=2".
std::string param(beast::string_view list, const std::string &key) {
  for (size_t i = 0; i <= list.size();) {
    size_t e = list.find('&', i);
    if (e == beast::string_view::npos)
      e = list.size();
    auto kv = list.substr(i, e - i);
    while (!kv.empty() && kv.front() == ' ')
      kv.remove_prefix(1);
    if (kv.size() > key.size() && kv.substr(0, key.size()) == key && kv[key.size()] == '=')
      return std::string(kv.substr(key.size() + 1));
    i = e + 1;
  }
  return {};
}

std::string query(beast::string_view target) {
  const auto q = target.find('?');
  return q == beast::string_view::npos ? std::string() : std::string(target.substr(q + 1));
}

bool token_ok(const ServerOptions &o, beast::string_view target) { return same(param(query(target), "t"), o.token); }

const char *mime(const std::string &ext) {
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".js") return "text/javascript";
  if (ext == ".css") return "text/css";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".json") return "application/json";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".map") return "application/json";
  return "application/octet-stream";
}

} // namespace

class WsClient : public std::enable_shared_from_this<WsClient> {
public:
  WsClient(tcp::socket &&s, Server &srv) : _ws(std::move(s)), _srv(srv) {}

  void accept(http::request<http::string_body> req) {
    _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    // a small socket buffer: a page that stops reading (its thread busy) fills it in a fraction of a
    // second, and the queue above then skips rows instead of the loopback buffering seconds of them
    beast::error_code ignored;
    beast::get_lowest_layer(_ws).socket().set_option(net::socket_base::send_buffer_size(256 * 1024), ignored);
    _ws.read_message_max(64 * 1024);
    _ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
      if (ec)
        return;
      self->_srv.add(self);
      if (self->_srv.handlers().on_open)
        self->_srv.handlers().on_open(self);
      self->read();
    });
  }

  // I/O thread only.
  void send(std::shared_ptr<const std::string> text, std::shared_ptr<const std::vector<uint8_t>> bin) {
    if (_closed)
      return;
    if (text && _q.size() >= kMaxQueuedText) {
      close();
      beast::error_code ignored;
      beast::get_lowest_layer(_ws).socket().close(ignored);
      return;
    }
    if (bin && _queued_bin >= kMaxQueuedBinary) {
      // a slow page: the oldest row still waiting goes, so what it gets next is the newest picture
      // (the front may be in flight: the one after it)
      _srv.count_drop();
      for (auto it = _q.begin() + 1; it != _q.end(); ++it)
        if (it->bin) {
          _q.erase(it);
          _queued_bin--;
          break;
        }
    }
    _q.push_back({std::move(text), std::move(bin)});
    if (_q.back().bin)
      _queued_bin++;
    if (!_writing)
      write();
  }

private:
  struct Out {
    std::shared_ptr<const std::string> text;
    std::shared_ptr<const std::vector<uint8_t>> bin;
  };

  void read() {
    _ws.async_read(_buf, [self = shared_from_this()](beast::error_code ec, size_t) {
      if (ec) {
        self->close();
        return;
      }
      if (self->_ws.got_text() && self->_srv.handlers().on_text) {
        const std::string s = beast::buffers_to_string(self->_buf.data());
        self->_srv.handlers().on_text(self, s);
      }
      self->_buf.consume(self->_buf.size());
      self->read();
    });
  }

  void write() {
    _writing = true;
    Out &o = _q.front();
    _ws.binary(o.bin != nullptr);
    auto done = [self = shared_from_this()](beast::error_code ec, size_t) {
      if (self->_q.front().bin)
        self->_queued_bin--;
      self->_q.pop_front();
      if (ec) {
        self->close();
        return;
      }
      if (!self->_q.empty())
        self->write();
      else
        self->_writing = false;
    };
    if (o.bin)
      _ws.async_write(net::buffer(*o.bin), std::move(done));
    else
      _ws.async_write(net::buffer(*o.text), std::move(done));
  }

  void close() {
    if (_closed)
      return;
    _closed = true;
    _srv.remove(shared_from_this());
  }

  websocket::stream<beast::tcp_stream> _ws;
  Server &_srv;
  beast::flat_buffer _buf;
  std::deque<Out> _q;
  size_t _queued_bin = 0;
  bool _writing = false, _closed = false;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(tcp::socket &&s, Server &srv, std::string webroot)
      : _stream(std::move(s)), _srv(srv), _root(std::move(webroot)) {}
  void run() { read(); }

private:
  void read() {
    _req = {};
    _stream.expires_after(std::chrono::seconds(30));
    http::async_read(_stream, _buf, _req, [self = shared_from_this()](beast::error_code ec, size_t) {
      if (ec)
        return;
      self->handle();
    });
  }

  template <class Body> void reply(http::response<Body> &&res) {
    auto sp = std::make_shared<http::response<Body>>(std::move(res));
    http::async_write(_stream, *sp, [self = shared_from_this(), sp](beast::error_code ec, size_t) {
      if (ec || !sp->keep_alive()) {
        beast::error_code ignored;
        self->_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
        return;
      }
      self->read();
    });
  }

  void text(http::status st, std::string body, const char *type = "text/plain") {
    http::response<http::string_body> res{st, _req.version()};
    res.set(http::field::content_type, type);
    res.set(http::field::cache_control, "no-store");
    res.keep_alive(_req.keep_alive());
    if (_req.method() != http::verb::head) // a HEAD response never carries a body
      res.body() = std::move(body);
    res.prepare_payload();
    reply(std::move(res));
  }

  void handle() {
    const ServerOptions &o = _srv.options();
    if (!host_ok(o, _req[http::field::host]))
      return text(http::status::forbidden, "bad host\n");
    const std::string target(_req.target());
    const std::string path = target.substr(0, target.find('?'));
    if (websocket::is_upgrade(_req)) {
      if (path != "/ws" || !token_ok(o, _req.target()) || !origin_ok(o, _req[http::field::origin]))
        return text(http::status::forbidden, "forbidden\n");
      std::make_shared<WsClient>(_stream.release_socket(), _srv)->accept(std::move(_req));
      return;
    }
    if (_req.method() != http::verb::get)
      return text(http::status::method_not_allowed, "GET only\n");
    if (path == "/api/hello")
      return text(http::status::ok, "{\"app\":\"wsdr-studio\",\"protocol\":1}\n", "application/json");
    if (path.find("..") != std::string::npos || path.find('%') != std::string::npos ||
        path.find('\\') != std::string::npos || path.empty() || path[0] != '/')
      return text(http::status::bad_request, "bad path\n");
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(_root, ec);
    fs::path file = fs::weakly_canonical(root / (path == "/" ? std::string("index.html") : path.substr(1)), ec);
    const std::string rs = root.string() + "/";
    if (ec || file.string().compare(0, rs.size(), rs) != 0 || !fs::is_regular_file(file))
      return text(http::status::not_found, "not found\n");
    http::file_body::value_type body;
    beast::error_code bec;
    body.open(file.c_str(), beast::file_mode::scan, bec);
    if (bec)
      return text(http::status::not_found, "not found\n");
    http::response<http::file_body> res{std::piecewise_construct, std::make_tuple(std::move(body)),
                                        std::make_tuple(http::status::ok, _req.version())};
    res.set(http::field::content_type, mime(file.extension().string()));
    res.set(http::field::cache_control, path.rfind("/assets/", 0) == 0 ? "max-age=31536000, immutable" : "no-cache");
    res.set("X-Content-Type-Options", "nosniff");
    res.keep_alive(_req.keep_alive());
    res.prepare_payload();
    reply(std::move(res));
  }

  beast::tcp_stream _stream;
  Server &_srv;
  std::string _root;
  beast::flat_buffer _buf;
  http::request<http::string_body> _req;
};

struct Server::Impl {
  tcp::acceptor acc;
  explicit Impl(net::io_context &io) : acc(io) {}
};

Server::Server(net::io_context &io, ServerOptions o, Handlers h)
    : _io(io), _o(std::move(o)), _h(std::move(h)), _impl(std::make_unique<Impl>(io)) {}

Server::~Server() = default;

bool Server::listen(std::string &err) {
  beast::error_code ec;
  const tcp::endpoint ep(net::ip::make_address("127.0.0.1"), _o.port);
  _impl->acc.open(ep.protocol(), ec);
  if (!ec)
    _impl->acc.set_option(net::socket_base::reuse_address(true), ec);
  if (!ec)
    _impl->acc.bind(ep, ec);
  if (!ec)
    _impl->acc.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    err = ec.message();
    beast::error_code ignored;
    _impl->acc.close(ignored);
    return false;
  }
  accept_next();
  return true;
}

void Server::accept_next() {
  // One I/O thread runs everything, so no strands are needed.
  _impl->acc.async_accept([this](beast::error_code ec, tcp::socket s) {
    if (!ec)
      std::make_shared<HttpSession>(std::move(s), *this, _o.webroot)->run();
    if (_impl->acc.is_open())
      accept_next();
  });
}

void Server::add(const ClientPtr &c) {
  std::lock_guard<std::mutex> l(_cm);
  _clients.insert(c);
  _nclients = _clients.size();
}

void Server::remove(const ClientPtr &c) {
  std::lock_guard<std::mutex> l(_cm);
  _clients.erase(c);
  _nclients = _clients.size();
}

void Server::broadcast_binary(std::vector<uint8_t> &&frame) {
  if (!_nclients)
    return;
  auto f = std::make_shared<const std::vector<uint8_t>>(std::move(frame));
  net::post(_io, [this, f] {
    std::set<ClientPtr> cs;
    {
      std::lock_guard<std::mutex> l(_cm);
      cs = _clients;
    }
    for (const auto &c : cs)
      c->send(nullptr, f);
  });
}

void Server::broadcast_text(std::string text) {
  if (!_nclients)
    return;
  auto t = std::make_shared<const std::string>(std::move(text));
  net::post(_io, [this, t] {
    std::set<ClientPtr> cs;
    {
      std::lock_guard<std::mutex> l(_cm);
      cs = _clients;
    }
    for (const auto &c : cs)
      c->send(t, nullptr);
  });
}

void Server::send_text(const std::weak_ptr<WsClient> &wc, std::string text) {
  auto t = std::make_shared<const std::string>(std::move(text));
  net::post(_io, [wc, t] {
    if (auto c = wc.lock())
      c->send(t, nullptr);
  });
}

} // namespace studio
