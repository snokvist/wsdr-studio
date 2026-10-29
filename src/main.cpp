// SPDX-License-Identifier: GPL-2.0-only
// wsdr-studiod — the wsdr-studio backend: owns the adapter (via wifi-sdr/devourer), computes the
// spectrum and serves the web UI on http://127.0.0.1:PORT/ to the desktop shell (desktop/, Electron).
// See server.h for the security model.
//
//   wsdr-studiod --ui-pipe [options]   started by the desktop shell: prints {"port","token"} as one JSON
//       line on stdout and exits when stdin closes. If an instance is already running it prints that
//       one's port and token with "attached":true and exits.
//   wsdr-studiod [options]             server only (scripts, tests); the token is in the instance file
//   options: [--port 8073] [--web DIR] [--pid 0xb812] [--freq 5190e6] [--rate 40] [--record-dir DIR]
//            [--no-usb3] (leave an 8812BU at USB 2; by default Start switches it to USB 3)
//            [--dev-origin http://localhost:5173]
//   wsdr-studiod --headless-seconds 20 --pid 0xb812 [--freq ..] [--rate ..] [--fft 8192]
//       engine + DSP only (no server); prints status JSON each second and a summary.
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "server.h"

using namespace studio;
namespace fs = std::filesystem;
namespace net = boost::asio;

namespace {

struct Args {
  uint16_t port = 8073;
  bool ui_pipe = false;
  bool usb3 = true; // switch an 8812BU found at USB 2 to USB 3 at Start
  std::string web, record_dir;
  std::vector<std::string> dev_origins;
  uint16_t pid = 0;
  double freq = 0, rate_msps = 0, headless = 0;
  int fft = 0;
};

int rate_sel_for(double msps) {
  int best = 5;
  for (int r = 1; r <= 5; r++)
    if (std::abs(80.0 / (1 << r) - msps) < std::abs(80.0 / (1 << best) - msps))
      best = r;
  return best;
}

std::string random_token() {
  std::random_device rd;
  char b[33];
  for (int i = 0; i < 16; i++)
    std::snprintf(b + 2 * i, 3, "%02x", rd() & 0xff);
  return b;
}

std::string exe_dir() {
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec).parent_path().string();
}

std::string default_webroot() {
  for (const fs::path &p : {fs::path(exe_dir()) / "../share/wsdr-studio/web", fs::path(WSDR_STUDIO_SOURCE_DIR) / "web/dist"}) {
    std::error_code ec;
    if (fs::exists(p / "index.html", ec))
      return fs::weakly_canonical(p, ec).string();
  }
  return (fs::path(WSDR_STUDIO_SOURCE_DIR) / "web/dist").string();
}

std::string data_dir() {
  const char *x = std::getenv("XDG_DATA_HOME");
  const char *h = std::getenv("HOME");
  return x && *x ? std::string(x) + "/wsdr-studio" : std::string(h ? h : "/tmp") + "/.local/share/wsdr-studio";
}

// Private per-user file "pid port token" for the running instance (never in a shared /tmp).
std::string private_dir();
std::string instance_file() { return private_dir() + "/instance"; }

bool write_instance(const std::string &text) {
  const int fd = ::open(instance_file().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  ::fchmod(fd, 0600);
  const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  ::close(fd);
  return ok;
}

struct Instance {
  long pid;
  unsigned port;
  std::string token;
};
// The running instance from the instance file, if its pid is alive, ours, and wsdr-studiod (a stale
// file after a crash is ignored).
std::optional<Instance> running_instance() {
  std::ifstream f(instance_file());
  long pid = 0;
  unsigned port = 0;
  std::string token;
  if (!(f >> pid >> port >> token) || port == 0 || port > 65535 || pid <= 0)
    return {};
  struct stat ps {};
  const std::string proc = "/proc/" + std::to_string(pid);
  std::ifstream comm(proc + "/comm");
  std::string name;
  if (::stat(proc.c_str(), &ps) != 0 || ps.st_uid != getuid() || !std::getline(comm, name) || name != "wsdr-studiod")
    return {};
  return Instance{pid, port, token};
}

// Remove the instance file only if it still describes us (another instance may have replaced it).
void remove_instance_if_ours(const std::string &token) {
  std::ifstream f(instance_file());
  long pid = 0;
  unsigned port = 0;
  std::string t;
  if (f >> pid >> port >> t && t == token) {
    std::error_code ec;
    fs::remove(instance_file(), ec);
  }
}

// Private per-user directory for the instance file (0700).
std::string private_dir() {
  const char *r = std::getenv("XDG_RUNTIME_DIR");
  const char *h = std::getenv("HOME");
  const std::string dir = r && *r ? std::string(r) + "/wsdr-studio" : std::string(h ? h : ".") + "/.cache/wsdr-studio";
  std::error_code ec;
  fs::create_directories(dir, ec);
  ::chmod(dir.c_str(), 0700);
  return dir;
}

bool parse(int argc, char **argv, Args &a) try {
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    auto v = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--port") {
      const int p = std::stoi(v());
      if (p < 1 || p > 65535 - 9)
        throw std::invalid_argument("port");
      a.port = static_cast<uint16_t>(p);
    }
    else if (k == "--ui-pipe") a.ui_pipe = true;
    else if (k == "--no-usb3") a.usb3 = false;
    else if (k == "--web") a.web = v();
    else if (k == "--record-dir") a.record_dir = v();
    else if (k == "--dev-origin") a.dev_origins.push_back(v());
    else if (k == "--pid") a.pid = static_cast<uint16_t>(std::stoul(v(), nullptr, 0));
    else if (k == "--freq") a.freq = std::stod(v());
    else if (k == "--rate") a.rate_msps = std::stod(v());
    else if (k == "--fft") a.fft = std::stoi(v());
    else if (k == "--headless-seconds") a.headless = std::stod(v());
    else if (k == "-h" || k == "--help") return false;
    else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return false;
    }
  }
  return true;
} catch (const std::exception &) {
  std::fprintf(stderr, "bad option value\n");
  return false;
}

json open_cmd(const Args &a) {
  json c = {{"cmd", "open"}, {"pid", a.pid}, {"id", 0}};
  if (a.freq > 0)
    c["freq"] = a.freq;
  if (a.rate_msps > 0)
    c["rate_sel"] = rate_sel_for(a.rate_msps);
  // From the command line, rates the link cannot carry fall back to the fastest continuous one (the
  // 8812CU's gapped spans are chosen in the app, where they are labelled).
  c["allow_gapped"] = false;
  c["usb3"] = a.usb3;
  if (wsdr::jaguar3_pid(a.pid) && a.rate_msps > 5)
    std::fprintf(stderr, "wsdr-studiod: the 8812CU / 8812EU stream up to 5 MS/s; wider spans (gapped) are in Span / rate\n");
  return c;
}

int run_headless(const Args &a) {
  if (!a.pid) {
    std::fprintf(stderr, "--headless-seconds needs --pid\n");
    return 2;
  }
  std::atomic<uint64_t> frames{0}, bytes{0};
  Engine eng({[&](std::vector<uint8_t> &&f) { frames++; bytes += f.size(); },
              [&](const json &e) {
                if (e.value("type", "") != "status")
                  std::printf("%s\n", e.dump().c_str());
              }},
             a.record_dir.empty() ? data_dir() + "/recordings" : a.record_dir);
  if (a.fft)
    eng.submit({{"cmd", "dsp"}, {"fft", a.fft}}, nullptr);
  std::atomic<int> open_rc{-1};
  eng.submit(open_cmd(a), [&](const json &r) {
    std::printf("%s\n", r.dump().c_str());
    open_rc = r.value("type", "") == "ack" ? 0 : 1;
  });
  while (open_rc < 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (open_rc != 0)
    return 3;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t f0 = 0;
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < a.headless) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    eng.poll();
    json s = eng.status();
    s["row_frames_per_s"] = frames - f0;
    f0 = frames;
    std::printf("%s\n", s.dump().c_str());
    std::fflush(stdout);
  }
  const json s = eng.status();
  std::printf("{\"ev\":\"headless\",\"rows\":%llu,\"frames\":%llu,\"bytes_per_s\":%.0f,\"ring_dropped\":%llu,"
              "\"dsp_load\":%.3f,\"state\":%s}\n",
              static_cast<unsigned long long>(eng.rows_emitted()), static_cast<unsigned long long>(frames.load()),
              bytes / a.headless, static_cast<unsigned long long>(eng.ring_dropped()), eng.dsp_load(),
              s["state"].dump().c_str());
  // A lost or failed adapter is closed by the engine, which leaves its reason in "error".
  const bool failed = s["state"] != "open" || !s.value("error", std::string()).empty();
  return failed ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: wsdr-studiod [--ui-pipe] [--port N] [--web DIR] [--pid 0xb812|0xc812|0xa81a] [--freq HZ] "
                         "[--rate MSPS] [--record-dir DIR] [--no-usb3] | --headless-seconds N --pid ...\n");
    return 2;
  }
  if (!a.record_dir.empty()) {
    std::error_code ec;
    a.record_dir = fs::absolute(a.record_dir, ec).string();
  }
  if (a.headless > 0)
    return run_headless(a);

  // Single instance (one process owns the adapters): the shell attaches to a running one.
  if (const auto inst = running_instance()) {
    if (a.ui_pipe) {
      std::printf("%s\n",
                  json{{"port", inst->port}, {"token", inst->token}, {"pid", inst->pid}, {"attached", true}}.dump().c_str());
      std::fflush(stdout);
      return 0;
    }
    std::fprintf(stderr, "wsdr-studiod: already running (pid %ld, port %u)\n", inst->pid, inst->port);
    return 1;
  }
  ::signal(SIGPIPE, SIG_IGN); // a closed stdout/stderr (the shell is gone) must not kill a finalising recording
  static std::atomic<bool> finished{false};
  if (a.ui_pipe) {
    prctl(PR_SET_PDEATHSIG, SIGTERM); // backup: the shell died (its pipe to our stdin also closes)
    // The shell stops us by closing our stdin; started now, so a shell that is already gone is noticed.
    std::thread([] {
      char b[256];
      while (::read(0, b, sizeof b) > 0) {
      }
      if (!finished)
        ::kill(::getpid(), SIGTERM);
    }).detach();
  }

  net::io_context io;
  ServerOptions so;
  so.token = random_token();
  so.webroot = a.web.empty() ? default_webroot() : a.web;
  so.extra_origins = a.dev_origins;
  std::unique_ptr<Server> srv;
  // Two engines: view slots A (0) and B (1), each owning at most one adapter. Commands name their slot
  // ("slot", default 0); events carry it; row frames carry it in the path byte (bit 4).
  constexpr int kSlots = 2;
  std::array<std::unique_ptr<Engine>, kSlots> engs;
  const auto tagged = [](json e, int slot) {
    e["slot"] = slot;
    return e.dump(-1, ' ', false, json::error_handler_t::replace); // (never throw on bad UTF-8)
  };
  Server::Handlers h;
  h.on_open = [&](const Server::ClientPtr &c) {
    std::weak_ptr<WsClient> w = c;
    const char *home = std::getenv("HOME");
    srv->send_text(w, json{{"type", "hello"}, {"version", 1}, {"app", "wsdr-studio"}, {"home", home ? home : ""},
                           {"slots", kSlots}}.dump());
    engs[0]->submit({{"cmd", "scan"}}, nullptr); // broadcasts "devices"; USB enumeration stays off the I/O thread
    for (int i = 0; i < kSlots; i++) {
      srv->send_text(w, tagged(engs[i]->caps(), i));
      srv->send_text(w, tagged(engs[i]->settings(), i));
      srv->send_text(w, tagged(engs[i]->status(), i));
    }
  };
  h.on_text = [&](const Server::ClientPtr &c, const std::string &t) {
    std::weak_ptr<WsClient> w = c;
    json cmd = json::parse(t, nullptr, false);
    if (cmd.is_discarded() || !cmd.is_object()) {
      srv->send_text(w, json{{"type", "error"}, {"kind", "bad_request"}, {"msg", "not a JSON object"}}.dump());
      return;
    }
    if (cmd.contains("cmd") && cmd["cmd"].is_string() && cmd["cmd"].get<std::string>().rfind('_', 0) == 0) {
      json e = {{"type", "error"}, {"kind", "bad_request"}, {"msg", "unknown command"}};
      if (cmd.contains("id"))
        e["id"] = cmd["id"];
      srv->send_text(w, e.dump());
      return; // internal commands (_lost, _rec_stop) are the engine's own
    }
    int slot = 0;
    if (cmd.contains("slot")) {
      if (!cmd["slot"].is_number_integer() || cmd["slot"].get<int>() < 0 || cmd["slot"].get<int>() >= kSlots) {
        json e = {{"type", "error"}, {"kind", "bad_request"}, {"msg", "slot must be 0 or 1"}};
        if (cmd.contains("id"))
          e["id"] = cmd["id"];
        srv->send_text(w, e.dump());
        return;
      }
      slot = cmd["slot"].get<int>();
    }
    // (a reply may carry a path with bytes that are not UTF-8: replaced, never thrown on)
    engs[slot]->submit(std::move(cmd), [&srv, w](const json &r) { srv->send_text(w, r.dump(-1, ' ', false, json::error_handler_t::replace)); });
  };
  std::string err;
  bool ok = false;
  for (int k = 0; k < 10 && !ok; k++) {
    so.port = static_cast<uint16_t>(a.port + k);
    srv = std::make_unique<Server>(io, so, h);
    ok = srv->listen(err);
  }
  if (!ok) {
    std::fprintf(stderr, "wsdr-studiod: cannot listen on 127.0.0.1:%u..%u: %s\n", a.port, a.port + 9, err.c_str());
    return 1;
  }
  const auto claims = std::make_shared<Claims>();
  for (int i = 0; i < kSlots; i++) {
    engs[i] = std::make_unique<Engine>(
        Engine::Callbacks{[&](std::vector<uint8_t> &&f) { srv->broadcast_binary(std::move(f)); },
                          [&, i](const json &e) {
                            // the adapter list is everyone's; the rest is this slot's
                            srv->broadcast_text(e.value("type", "") == "devices" ? e.dump() : tagged(e, i));
                          }},
        a.record_dir.empty() ? data_dir() + "/recordings" : a.record_dir, i, claims);
    engs[i]->set_usb3_default(a.usb3); // also for Start pressed in the app
  }
  Engine *eng = engs[0].get(); // the command line opens view A

  if (!write_instance(std::to_string(getpid()) + " " + std::to_string(srv->port()) + " " + so.token + "\n"))
    std::fprintf(stderr, "wsdr-studiod: cannot write %s (a second launch will not find this one)\n",
                 instance_file().c_str());
  std::fprintf(stderr, "wsdr-studiod: serving http://127.0.0.1:%u/ from %s\n", srv->port(), so.webroot.c_str());
  if (a.pid) // (no client is there yet for the reply: a failure goes to stderr and the view's status)
    eng->submit(open_cmd(a), [eng](const json &r) {
      if (r.value("type", "") != "error")
        return;
      const std::string msg = "--pid/--freq/--rate: " + r.value("msg", "");
      std::fprintf(stderr, "wsdr-studiod: %s\n", msg.c_str());
      eng->note_failure(msg);
    });

  net::signal_set sig(io, SIGINT, SIGTERM, SIGHUP); // SIGHUP: the terminal closed; finalise cleanly
  sig.async_wait([&](const boost::system::error_code &, int) { io.stop(); });

  if (a.ui_pipe) {
    // The shell reads this one line (a private pipe: the token is never on a command line).
    std::printf("%s\n", json{{"port", srv->port()}, {"token", so.token}}.dump().c_str());
    std::fflush(stdout);
    const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC); // nothing else may reach that pipe
    if (null >= 0) {
      ::dup2(null, 1);
      ::close(null);
    }
  }

  // 2 Hz status to all clients.
  net::steady_timer tick(io);
  std::function<void()> on_tick = [&] {
    tick.expires_after(std::chrono::milliseconds(500));
    tick.async_wait([&](const boost::system::error_code &ec) {
      if (ec)
        return;
      for (int i = 0; i < kSlots; i++) {
        engs[i]->poll(); // notices an unplugged adapter
        if (srv->clients()) {
          json st = engs[i]->status();
          st["client_drops"] = srv->frames_dropped();
          srv->broadcast_text(tagged(st, i));
        }
      }
      on_tick();
    });
  };
  on_tick();
  io.run();
  finished = true; // a late stdin EOF must not signal us during the shutdown below

  remove_instance_if_ours(so.token);
  for (auto &e : engs)
    e.reset(); // closes the adapters before the server goes away
  srv.reset();
  return 0;
}
