// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/workbench.hpp"

#include <arpa/inet.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "glassbox/demo.hpp"
#include "glassbox/json.hpp"
#include "glassbox/recorder.hpp"
#include "glassbox/trace_io.hpp"
#include "ui/assets.hpp"
#include "ui/http.hpp"
#include "ui/live.hpp"
#include "ui/routes.hpp"
#include "platform/platform.hpp"
#include "ui/serialize.hpp"

extern char** environ;

namespace gbx {
namespace {

constexpr size_t kMaxTraceBytes = 2ull << 30;

uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string random_token() {
  unsigned char bytes[16] = {};
  platform::random_bytes(bytes, sizeof(bytes));
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (unsigned char byte : bytes) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0f]);
  }
  return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string cookie_value(const ui::HttpRequest& request, std::string_view name) {
  const std::string cookie = request.header("cookie");
  size_t pos = 0;
  while (pos < cookie.size()) {
    const size_t semicolon = cookie.find(';', pos);
    const size_t end = semicolon == std::string::npos ? cookie.size() : semicolon;
    std::string_view item(cookie.data() + pos, end - pos);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.remove_prefix(1);
    }
    const size_t equals = item.find('=');
    if (equals != std::string_view::npos &&
        item.substr(0, equals) == name) {
      return std::string(item.substr(equals + 1));
    }
    if (semicolon == std::string::npos) break;
    pos = semicolon + 1;
  }
  return {};
}

bool host_allowed(const ui::HttpRequest& request, uint16_t port) {
  std::string host = request.header("host");
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) host.resize(colon);
  if (host != "127.0.0.1" && host != "localhost") return false;
  (void)port;
  return true;
}

bool origin_allowed(const ui::HttpRequest& request, uint16_t port) {
  const std::string origin = request.header("origin");
  if (origin.empty()) return true;
  const std::string expected = "http://127.0.0.1:" + std::to_string(port);
  const std::string localhost = "http://localhost:" + std::to_string(port);
  return origin == expected || origin == localhost ||
         origin == "http://127.0.0.1" || origin == "http://localhost";
}

std::string filename_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ends_with_gbt(const std::string& path) {
  if (path.size() < 4) return false;
  std::string suffix = path.substr(path.size() - 4);
  for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return suffix == ".gbt";
}

bool path_inside(const std::string& path, const std::string& root) {
  if (path == root) return true;
  if (path.size() <= root.size()) return false;
  if (path.compare(0, root.size(), root) != 0) return false;
  return path[root.size()] == '/';
}

struct FileStamp {
  uint64_t bytes = 0;
  int64_t mtime = 0;
};

bool stat_file(const std::string& path, FileStamp& stamp) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
  stamp.bytes = static_cast<uint64_t>(info.st_size);
  stamp.mtime = static_cast<int64_t>(info.st_mtime);
  return true;
}

std::string json_error(std::string_view message) {
  json::Writer writer;
  writer.begin_object();
  writer.key_string("error", message);
  writer.end_object();
  return writer.str();
}

}  // namespace

struct Workbench::Impl {
  explicit Impl(WorkbenchOptions opts) : options(std::move(opts)) {}

  WorkbenchOptions options;
  std::unique_ptr<ui::HttpServer> server;
  std::thread serve_thread;
  std::atomic<bool> stopping{false};
  std::atomic<bool> running{false};
  std::atomic<bool> shutdown_requested{false};
  std::atomic<bool> demo_running{false};
  std::string token;
  std::string trace_root;
  std::vector<std::string> roots;
  std::mutex trace_mu;
  ui::TraceSnapshot current;
  mutable std::mutex error_mu;
  Status error;
  uint64_t started_ms = 0;
  ui::LiveFeed live;
  std::thread demo_thread;
  std::mutex demo_mu;

  void set_error(Status status) {
    std::lock_guard lock(error_mu);
    error = std::move(status);
  }

  ui::TraceSnapshot snapshot() {
    std::lock_guard lock(trace_mu);
    return current;
  }

  bool authorized(const ui::HttpRequest& request, ui::HttpResponse& response,
                  bool allow_cookie) {
    if (!host_allowed(request, server ? server->port() : 0) ||
        !origin_allowed(request, server ? server->port() : 0)) {
      response.set_error(403, "host or origin not allowed");
      return false;
    }
    std::string candidate = request.header("x-glassbox-token");
    if (candidate.empty()) candidate = request.query_value("t");
    if (candidate.empty() && allow_cookie) {
      candidate = cookie_value(request, "gb_token");
    }
    if (!constant_time_equal(candidate, token)) {
      response.set_error(403, "missing or invalid session token");
      return false;
    }
    return true;
  }

  ui::Guard guard() {
    return [this](const ui::HttpRequest& request, ui::HttpResponse& response) {
      return authorized(request, response, true);
    };
  }

  void add_static(ui::HttpServer& http, std::string_view path,
                  std::string_view asset_name) {
    http.route("GET", path,
               [this, asset_name](const ui::HttpRequest& request,
                                  ui::HttpResponse& response) {
                 if (!authorized(request, response, true)) return;
                 const auto* asset = ui::assets::find(asset_name);
                 if (asset == nullptr) {
                   response.set_error(404, "asset missing");
                   return;
                 }
                 std::string type = "application/octet-stream";
                 if (asset_name.size() >= 5 &&
                     asset_name.substr(asset_name.size() - 5) == ".html") {
                   type = "text/html; charset=utf-8";
                   response.headers.emplace_back(
                       "Content-Security-Policy",
                       "default-src 'self'; script-src 'self'; style-src 'self' "
                       "'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
                       "base-uri 'none'; form-action 'none'; "
                       "frame-ancestors 'none'");
                 } else if (asset_name.size() >= 3 &&
                            asset_name.substr(asset_name.size() - 3) == ".js") {
                   type = "application/javascript; charset=utf-8";
                 } else if (asset_name.size() >= 4 &&
                            asset_name.substr(asset_name.size() - 4) == ".css") {
                   type = "text/css; charset=utf-8";
                 } else if (asset_name.size() >= 4 &&
                            asset_name.substr(asset_name.size() - 4) == ".svg") {
                   type = "image/svg+xml";
                 }
                 response.set_body(std::string(asset->data), type);
                 if (!request.query_value("t").empty()) {
                   response.headers.emplace_back(
                       "Set-Cookie", "gb_token=" + token +
                                         "; Path=/; HttpOnly; SameSite=Strict");
                 }
               });
  }

  void add_index(ui::HttpServer& http, std::string_view path) {
    http.route("GET", path,
               [this](const ui::HttpRequest& request,
                      ui::HttpResponse& response) {
                 if (!authorized(request, response, true)) return;
                 const auto* asset = ui::assets::find("index.html");
                 if (asset == nullptr) {
                   response.set_error(500, "index asset missing");
                   return;
                 }
                 response.set_body(std::string(asset->data),
                                   "text/html; charset=utf-8");
                 response.headers.emplace_back(
                     "Content-Security-Policy",
                     "default-src 'self'; script-src 'self'; style-src 'self' "
                     "'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
                     "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
                 if (!request.query_value("t").empty()) {
                   response.headers.emplace_back(
                       "Set-Cookie", "gb_token=" + token +
                                         "; Path=/; HttpOnly; SameSite=Strict");
                 }
               });
  }

  bool allowed_by_roots(const std::string& path) const {
    for (const auto& root : roots) {
      if (path_inside(path, root)) return true;
    }
    return false;
  }

  Status resolve_trace_path(std::string_view input, std::string& canonical,
                            FileStamp& stamp) {
    if (input.empty() || input.size() > 4096) {
      return Status::error(ErrorCode::InvalidArgument, "invalid trace path");
    }
    if (input.find('\0') != std::string_view::npos) {
      return Status::error(ErrorCode::InvalidArgument, "invalid trace path");
    }
    std::error_code ec;
    const auto path = std::filesystem::weakly_canonical(
        std::filesystem::path(std::string(input)), ec);
    if (ec) {
      return Status::error(ErrorCode::NotFound, "cannot resolve trace path");
    }
    canonical = path.string();
    if (!stat_file(canonical, stamp)) {
      return Status::error(ErrorCode::NotFound, "trace file not found");
    }
    if (!ends_with_gbt(canonical)) {
      return Status::error(ErrorCode::InvalidArgument,
                           "only .gbt trace files can be opened");
    }
    if (stamp.bytes > kMaxTraceBytes) {
      return Status::error(ErrorCode::LimitExceeded, "trace file is too large");
    }
    if (allowed_by_roots(canonical)) return Status::success();
    return Status::error(ErrorCode::PermissionDenied,
                         "trace is outside the allowed directories");
  }

  Status load_and_set(const std::string& path) {
    std::string canonical;
    FileStamp stamp;
    auto status = resolve_trace_path(path, canonical, stamp);
    if (!status.ok()) return status;
    auto trace = std::make_shared<Trace>();
    status = load_trace(canonical, *trace);
    if (!status.ok()) return status;
    const auto loaded = std::make_shared<const Trace>(std::move(*trace));
    {
      std::lock_guard lock(trace_mu);
      current.trace = loaded;
      current.path = canonical;
      current.name = filename_of(canonical);
      current.bytes = stamp.bytes;
      current.mtime = stamp.mtime;
    }
    return Status::success();
  }

  void add_api_routes(ui::HttpServer& http) {
    const ui::Guard check = guard();

    http.route("GET", "/api/health",
               [this, check](const ui::HttpRequest& request,
                             ui::HttpResponse& response) {
                 if (!check(request, response)) return;
                 const bool trace_loaded = snapshot().trace != nullptr;
                 json::Writer writer;
                 writer.begin_object();
                 writer.key_boolean("ok", true);
                 writer.key_string("version", GBX_VERSION);
                 writer.key_string("app", options.app_name);
                 writer.key_integer("port",
                                    static_cast<int64_t>(server->port()));
                 writer.key_integer("uptime_ms",
                                    static_cast<int64_t>(now_ms() - started_ms));
                 writer.key_boolean("trace_loaded", trace_loaded);
                 writer.key_boolean("recording", demo_running.load());
                 writer.key_integer("live_dropped",
                                    static_cast<int64_t>(live.dropped()));
#if defined(__APPLE__)
                 writer.key_string("platform", "macos");
                 writer.key_string("sampler", "mach");
#else
                 writer.key_string("platform", "linux");
                 writer.key_string("sampler", "manual");
#endif
                 writer.end_object();
                 response.set_body(writer.str());
               });

    http.route("GET", "/api/traces",
               [this, check](const ui::HttpRequest& request,
                             ui::HttpResponse& response) {
                 if (!check(request, response)) return;
                 struct Entry {
                   std::string path;
                   std::string name;
                   uint64_t bytes = 0;
                   int64_t mtime = 0;
                 };
                 std::vector<Entry> entries;
                 for (const auto& root : roots) {
                   std::error_code ec;
                   std::filesystem::directory_iterator iterator(root, ec);
                   if (ec) continue;
                   for (const auto& item : iterator) {
                     if (entries.size() >= 200) break;
                     if (!ends_with_gbt(item.path().string())) continue;
                     std::error_code resolve_error;
                     const auto resolved = std::filesystem::weakly_canonical(
                         item.path(), resolve_error);
                     if (resolve_error) continue;
                     const std::string path = resolved.string();
                     if (!allowed_by_roots(path)) continue;
                     FileStamp stamp;
                     if (!stat_file(path, stamp)) continue;
                     if (stamp.bytes > kMaxTraceBytes) continue;
                     entries.push_back(
                         {path, filename_of(path), stamp.bytes, stamp.mtime});
                   }
                 }
                 std::sort(entries.begin(), entries.end(),
                           [](const Entry& a, const Entry& b) {
                             if (a.mtime != b.mtime) return a.mtime > b.mtime;
                             return a.path < b.path;
                           });
                 json::Writer writer;
                 writer.begin_object();
                 const auto snap = snapshot();
                 writer.key_string("current", snap.path);
                 writer.key("traces");
                 writer.begin_array();
                 for (const auto& entry : entries) {
                   writer.begin_object();
                   writer.key_string("name", entry.name);
                   writer.key_string("path", entry.path);
                   writer.key_integer("bytes",
                                      static_cast<int64_t>(entry.bytes));
                   writer.key_integer("mtime", entry.mtime);
                   writer.end_object();
                 }
                 writer.end_array();
                 writer.end_object();
                 response.set_body(writer.str());
               });

    http.route("POST", "/api/open",
               [this, check](const ui::HttpRequest& request,
                             ui::HttpResponse& response) {
                 if (!check(request, response)) return;
                 json::Value body;
                 auto status = json::parse(request.body, body);
                 if (!status.ok()) {
                   response.set_error(400, "invalid JSON body: " + status.message());
                   return;
                 }
                 const auto* path_value = body.find("path");
                 if (path_value == nullptr || !path_value->is_string()) {
                   response.set_error(400, "body must contain a string path");
                   return;
                 }
                 status = load_and_set(path_value->string);
                 if (!status.ok()) {
                   response.set_error(400, status.describe());
                   return;
                 }
                 const auto snap = snapshot();
                 json::Writer writer;
                 writer.begin_object();
                 writer.key_boolean("ok", true);
                 writer.key_string("path", snap.path);
                 writer.key_string("name", snap.name);
                 writer.key_integer("records",
                                    static_cast<int64_t>(snap.trace->size()));
                 writer.key_integer(
                     "duration_ns",
                     static_cast<int64_t>(snap.trace->end_ts()));
                 writer.end_object();
                 response.set_body(writer.str());
               });

    http.route("POST", "/api/demo",
               [this, check](const ui::HttpRequest& request,
                             ui::HttpResponse& response) {
                 if (!check(request, response)) return;
                 bool already = demo_running.load();
                 if (already) {
                   response.set_error(409, "a demo recording is already running");
                   return;
                 }
                 DemoOptions demo;
                 demo.app_name = options.app_name;
                 if (!request.body.empty()) {
                   json::Value body;
                   auto status = json::parse(request.body, body);
                   if (!status.ok()) {
                     response.set_error(400, "invalid JSON body");
                     return;
                   }
                   if (const auto* frames = body.find("frames")) {
                     demo.frames = static_cast<uint32_t>(
                         std::clamp<int64_t>(static_cast<int64_t>(frames->as_number(90)),
                                             10, 600));
                   }
                   if (const auto* sampler = body.find("sampler")) {
                     demo.use_mach_sampler = sampler->as_boolean(false);
                   }
                   if (const auto* sleep = body.find("sleep_spike")) {
                     demo.sleep_spike = sleep->as_boolean(true);
                   }
                   if (const auto* seed = body.find("seed")) {
                     demo.seed = static_cast<uint32_t>(
                         std::clamp<int64_t>(
                             static_cast<int64_t>(seed->as_number(7)), 0,
                             0xffffffffll));
                   }
                 }
                 start_demo(demo);
                 json::Writer writer;
                 writer.begin_object();
                 writer.key_boolean("started", true);
                 writer.key_string("stream", "/api/live");
                 writer.end_object();
                 response.set_body(writer.str());
               });

    http.route("POST", "/api/shutdown",
               [this, check](const ui::HttpRequest& request,
                             ui::HttpResponse& response) {
                 if (!check(request, response)) return;
                 shutdown_requested.store(true);
                 stopping.store(true);
                 live.stop();
                 json::Writer writer;
                 writer.begin_object();
                 writer.key_boolean("ok", true);
                 writer.end_object();
                 response.set_body(writer.str());
                 server->request_stop();
               });

    register_trace_routes(http, [this] { return snapshot(); }, check);

    http.stream_route("/api/live", [this](const ui::HttpRequest& request,
                                          ui::ConnectionStream& stream) {
      ui::HttpResponse auth;
      if (!authorized(request, auth, true)) {
        stream.reject(403, "missing or invalid session token");
        return;
      }
      uint64_t cursor = 0;
      std::vector<ui::LiveFeed::Event> events;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::hours(1);
      auto last_keepalive = std::chrono::steady_clock::now();
      while (stream.alive() && !stopping.load() &&
             std::chrono::steady_clock::now() < deadline) {
        bool done = false;
        events.clear();
        const bool produced = live.poll(cursor, events, done, 500);
        bool saw_done = false;
        for (const auto& event : events) {
          if (event.name == "done") saw_done = true;
          if (!stream.sse_event(event.name, event.data)) return;
        }
        if (saw_done) return;
        if (done && !produced) {
          stream.sse_event("done", "{}");
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_keepalive > std::chrono::seconds(10)) {
          if (!stream.comment("keepalive")) return;
          last_keepalive = now;
        }
      }
    });

    add_index(http, "/");
    add_index(http, "/index.html");
    add_static(http, "/app.js", "app.js");
    add_static(http, "/style.css", "style.css");
    add_static(http, "/favicon.svg", "favicon.svg");
  }

  void start_demo(const DemoOptions& demo) {
    {
      std::lock_guard lock(demo_mu);
      if (demo_thread.joinable()) demo_thread.join();
      live.reset();
      demo_running.store(true);
    }
    demo_thread = std::thread([this, demo] { run_demo_job(demo); });
  }

  void run_demo_job(DemoOptions demo) {
    const std::string path =
        trace_root.empty() ? std::string("demo-live.gbt")
                           : trace_root + "/demo-live.gbt";
    Recorder recorder;
    recorder.set_live_sink([this](const LiveRecord& record) {
      if (stopping.load()) return;
      live.publish("record", ui::live_record_json(record));
    });
    auto status = recorder.start(path);
    if (!status.ok()) {
      demo_running.store(false);
      live.finish(json_error(status.describe()));
      return;
    }
    DemoResult result;
    status = run_demo(recorder, demo, result);
    if (!status.ok()) {
      recorder.cancel();
      demo_running.store(false);
      live.finish(json_error(status.describe()));
      return;
    }
    status = recorder.finish();
    if (!status.ok()) {
      demo_running.store(false);
      live.finish(json_error(status.describe()));
      return;
    }
    auto load_status = load_and_set(path);
    json::Writer writer;
    writer.begin_object();
    writer.key_boolean("ok", load_status.ok());
    if (load_status.ok()) {
      const auto snap = snapshot();
      writer.key_string("path", snap.path);
      writer.key_string("name", snap.name);
      writer.key_integer("records",
                         static_cast<int64_t>(snap.trace->size()));
      writer.key_integer("frames",
                         static_cast<int64_t>(result.frames_recorded));
      writer.key_integer("allocations",
                         static_cast<int64_t>(result.allocations));
      writer.key_integer("leaks", static_cast<int64_t>(result.leaks));
      writer.key_integer("samples", static_cast<int64_t>(result.samples));
      writer.key_integer("duration_ns",
                         static_cast<int64_t>(snap.trace->end_ts()));
      writer.key_integer("live_dropped",
                         static_cast<int64_t>(live.dropped()));
    } else {
      writer.key_string("error", load_status.describe());
    }
    writer.end_object();
    demo_running.store(false);
    live.finish(writer.str());
  }

  void prepare_roots() {
    roots.clear();
    trace_root = options.trace_dir.empty() ? std::string("traces")
                                           : options.trace_dir;
    std::error_code ec;
    std::filesystem::create_directories(trace_root, ec);
    const auto canonical =
        std::filesystem::weakly_canonical(std::filesystem::path(trace_root), ec);
    if (!ec) {
      trace_root = canonical.string();
      roots.push_back(trace_root);
    } else {
      roots.push_back(std::filesystem::absolute(trace_root).string());
    }
    for (const auto& dir : options.extra_trace_dirs) {
      std::error_code inner;
      const auto path = std::filesystem::weakly_canonical(
          std::filesystem::path(dir), inner);
      if (inner) continue;
      if (std::filesystem::is_directory(path, inner) && !inner) {
        const std::string text = path.string();
        if (text != trace_root) roots.push_back(text);
      }
    }
  }

  std::string newest_trace() {
    std::string best;
    int64_t best_mtime = std::numeric_limits<int64_t>::min();
    for (const auto& root : roots) {
      std::error_code ec;
      std::filesystem::directory_iterator iterator(root, ec);
      if (ec) continue;
      for (const auto& item : iterator) {
        if (!ends_with_gbt(item.path().string())) continue;
        std::error_code resolve_error;
        const auto resolved =
            std::filesystem::weakly_canonical(item.path(), resolve_error);
        if (resolve_error) continue;
        const std::string path = resolved.string();
        if (!allowed_by_roots(path)) continue;
        FileStamp stamp;
        if (!stat_file(path, stamp)) continue;
        if (stamp.bytes > kMaxTraceBytes) continue;
        if (stamp.mtime > best_mtime) {
          best_mtime = stamp.mtime;
          best = path;
        }
      }
    }
    return best;
  }

  Status start() {
    if (server) {
      return Status::error(ErrorCode::Busy, "workbench already started");
    }
    token = random_token();
    started_ms = now_ms();
    prepare_roots();

    server = std::make_unique<ui::HttpServer>();
    add_api_routes(*server);

    uint16_t bound_port = 0;
    Status listen_status;
    if (options.port != 0) {
      listen_status = server->listen(options.host, options.port);
    } else {
      for (uint16_t port = 7331; port <= 7340; ++port) {
        listen_status = server->listen(options.host, port);
        if (listen_status.ok()) break;
      }
      if (!listen_status.ok()) {
        listen_status = server->listen(options.host, 0);
      }
    }
    if (!listen_status.ok()) {
      server.reset();
      set_error(listen_status);
      return listen_status;
    }
    bound_port = server->port();
    (void)bound_port;

    if (!options.initial_trace.empty()) {
      const auto status = load_and_set(options.initial_trace);
      if (!status.ok()) set_error(wrap(status, "opening initial trace"));
    } else {
      const std::string newest = newest_trace();
      if (!newest.empty()) {
        const auto status = load_and_set(newest);
        if (!status.ok()) set_error(wrap(status, "opening newest trace"));
      }
    }
    if (snapshot().trace == nullptr && options.auto_record_demo) {
      DemoOptions demo;
      demo.app_name = options.app_name;
      const std::string path = trace_root + "/demo.gbt";
      const DemoResult result = record_demo_trace(path, demo);
      if (result.status.ok()) {
        const auto status = load_and_set(path);
        if (!status.ok()) set_error(wrap(status, "opening demo trace"));
      } else {
        set_error(wrap(result.status, "recording demo trace"));
      }
    }

    stopping.store(false);
    shutdown_requested.store(false);
    running.store(true);
    serve_thread = std::thread([this] { server->serve(); });
    if (options.open_browser) {
      open_in_browser("http://127.0.0.1:" + std::to_string(server->port()) +
                      "/?t=" + token);
    }
    return Status::success();
  }

  void stop() {
    if (!server) return;
    stopping.store(true);
    shutdown_requested.store(true);
    live.stop();
    {
      std::lock_guard lock(demo_mu);
      if (demo_thread.joinable()) demo_thread.join();
    }
    server->request_stop();
    if (serve_thread.joinable()) serve_thread.join();
    server->wait();
    running.store(false);
    server.reset();
  }
};

bool open_in_browser(std::string_view url) {
  pid_t pid = 0;
  std::string target(url);
  char* argv[] = {const_cast<char*>(platform::browser_command()), target.data(),
                  nullptr};
  const int rc = ::posix_spawnp(&pid, platform::browser_command(), nullptr,
                                nullptr, argv, environ);
  return rc == 0;
}

Workbench::Workbench(WorkbenchOptions options)
    : impl_(new Impl(std::move(options))) {}

Workbench::~Workbench() {
  if (impl_) impl_->stop();
}

Status Workbench::start() { return impl_->start(); }

void Workbench::stop() { impl_->stop(); }

bool Workbench::running() const {
  return impl_ && impl_->running.load() && !impl_->shutdown_requested.load();
}

uint16_t Workbench::port() const {
  return impl_ && impl_->server ? impl_->server->port() : 0;
}

std::string Workbench::url() const {
  if (!impl_ || !impl_->server) return {};
  return "http://127.0.0.1:" + std::to_string(impl_->server->port()) +
         "/?t=" + impl_->token;
}

std::string Workbench::token() const { return impl_ ? impl_->token : ""; }

Status Workbench::open_trace(std::string_view path) {
  if (!impl_) return Status::error(ErrorCode::Internal, "no workbench");
  return impl_->load_and_set(std::string(path));
}

Status Workbench::last_error() const {
  if (!impl_) return Status::error(ErrorCode::Internal, "no workbench");
  std::lock_guard lock(impl_->error_mu);
  return impl_->error;
}

}  // namespace gbx
