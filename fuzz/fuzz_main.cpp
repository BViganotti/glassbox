// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
//
// Deterministic fuzz harnesses. Not a libFuzzer integration (no dependencies):
// each mode applies a fixed-seed mutation strategy so ctest runs are
// reproducible and fast. Modes: reader, query, json, http, roundtrip.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "glassbox/analysis.hpp"
#include "glassbox/demo.hpp"
#include "glassbox/json.hpp"
#include "glassbox/query.hpp"
#include "glassbox/recorder.hpp"
#include "glassbox/trace_io.hpp"
#include "trace/file_io.hpp"
#include "ui/http.hpp"

namespace {

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  size_t below(size_t limit) { return limit == 0 ? 0 : next() % limit; }
  uint8_t byte() { return static_cast<uint8_t>(next() & 0xff); }
};

std::string temp_path(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("glassbox-fuzz-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

std::vector<uint8_t> read_bytes(const std::string& path) {
  std::vector<uint8_t> data;
  gbx::trace::read_file(path, 64u << 20, data);
  return data;
}

bool trace_invariants(const gbx::Trace& trace) {
  if (!trace.finalized()) return false;
  for (size_t i = 1; i < trace.records.size(); ++i) {
    if (trace.records[i - 1].ts > trace.records[i].ts) return false;
  }
  for (const auto& record : trace.records) {
    const auto fields = record.field_span(trace.fields);
    if (fields.size() != record.field_count) return false;
    for (const auto& field : fields) {
      if (field.key >= trace.strings.size()) return false;
      if (field.value.type == gbx::ValueType::String &&
          field.value.s >= trace.strings.size()) {
        return false;
      }
    }
  }
  return true;
}

int fuzz_reader(int iterations) {
  const std::string path = temp_path("base.gbt");
  gbx::DemoOptions options;
  options.frames = 6;
  options.sleep_spike = false;
  auto result = gbx::record_demo_trace(path, options);
  if (!result.status.ok()) return 1;
  const auto base = read_bytes(path);
  if (base.empty()) return 1;
  Rng rng(0x5ead);
  size_t accepted = 0;
  size_t rejected = 0;
  for (int i = 0; i < iterations; ++i) {
    std::vector<uint8_t> data = base;
    switch (rng.below(5)) {
      case 0: {
        const size_t flips = 1 + rng.below(8);
        for (size_t f = 0; f < flips && !data.empty(); ++f) {
          data[rng.below(data.size())] ^= static_cast<uint8_t>(1u << rng.below(8));
        }
        break;
      }
      case 1:
        data.resize(rng.below(data.size() + 1));
        break;
      case 2: {
        const size_t at = rng.below(data.size() + 1);
        const size_t count = 1 + rng.below(32);
        for (size_t c = 0; c < count; ++c) {
          data.insert(data.begin() + static_cast<ptrdiff_t>(at), rng.byte());
        }
        break;
      }
      case 3: {
        if (data.size() > 16) {
          const size_t at = rng.below(data.size());
          const size_t count = 1 + rng.below(data.size() - at);
          data.erase(data.begin() + static_cast<ptrdiff_t>(at),
                     data.begin() + static_cast<ptrdiff_t>(at + count));
        }
        break;
      }
      case 4: {
        data.resize(1 + rng.below(4096));
        for (auto& byte : data) byte = rng.byte();
        break;
      }
    }
    gbx::LoadOptions load;
    load.recover_truncated = (i % 3) != 0;
    load.verify_checksums = (i % 5) != 0;
    gbx::Trace trace;
    const auto status = gbx::load_trace_bytes(data, trace, load, "fuzz.gbt");
    if (!status.ok()) {
      ++rejected;
      continue;
    }
    if (i % 2 == 0) {
      // Exercise the streaming, file-backed loader with the same bytes.
      const std::string mutated_path = temp_path("mutated.gbt");
      gbx::trace::FileWriter writer;
      if (!writer.open(mutated_path).ok() || !writer.write(data).ok() ||
          !writer.close().ok()) {
        return 1;
      }
      gbx::Trace streamed;
      auto stream_status = gbx::load_trace(mutated_path, streamed, load);
      if (stream_status.ok()) {
        if (!trace_invariants(streamed)) {
          std::fprintf(stderr, "stream reader invariant failure at %d\n", i);
          return 1;
        }
      } else if (stream_status.code() == gbx::ErrorCode::Internal) {
        std::fprintf(stderr, "stream reader internal failure at %d: %s\n", i,
                     stream_status.describe().c_str());
        return 1;
      }
    }
    ++accepted;
    if (!trace_invariants(trace)) {
      std::fprintf(stderr, "reader fuzz: invariant failure at iteration %d\n", i);
      return 1;
    }
    if (trace.empty()) continue;
    const uint64_t end = trace.end_ts();
    gbx::build_timeline(trace, 64);
    gbx::analyze_spans(trace);
    gbx::active_spans(trace, end / 2);
    gbx::HeapOptions heap_options;
    heap_options.max_objects = 100;
    heap_options.max_sites = 32;
    gbx::heap_at(trace, end / 2, heap_options);
    gbx::leak_candidates(trace, 16);
    gbx::state_at(trace, end);
    gbx::FlameOptions flame_options;
    flame_options.max_nodes = 2000;
    gbx::build_flame(trace, {}, flame_options);
    gbx::HeapGraphOptions graph_options;
    graph_options.max_nodes = 200;
    graph_options.max_edges = 1000;
    gbx::build_heap_graph(trace, end, graph_options);
    gbx::diff(trace, 0, end, 32);
    gbx::QueryError error;
    auto query = gbx::Query::parse("type=alloc", &error);
    if (query.valid()) gbx::run_query(trace, query, 0, end, 32);
  }
  std::fprintf(stderr, "reader: %zu accepted, %zu rejected\n", accepted, rejected);
  return 0;
}

int fuzz_query(int iterations) {
  const std::string path = temp_path("query-base.gbt");
  gbx::DemoOptions options;
  options.frames = 8;
  options.sleep_spike = false;
  auto result = gbx::record_demo_trace(path, options);
  if (!result.status.ok()) return 1;
  gbx::Trace trace;
  if (!gbx::load_trace(path, trace).ok()) return 1;

  const char* seeds[] = {
      "type:alloc", "size>1024 AND site~asset", "msg~\"error\" OR level>3",
      "ts>1ms", "NOT type=log", "((thread=1))", "name~Frame",
      "inventory", "class=Texture AND size>=32768", "level=error",
  };
  Rng rng(0x51e2a7);
  const std::string alphabet =
      "type=logalloc size>1024<=( )ANDORNOT:~\"'-_abc123.msgthreadts.";
  size_t valid = 0;
  size_t evaluated = 0;
  for (int i = 0; i < iterations; ++i) {
    std::string text;
    if (i % 4 == 0) {
      text = seeds[rng.below(sizeof(seeds) / sizeof(seeds[0]))];
      const size_t mutations = 1 + rng.below(4);
      for (size_t m = 0; m < mutations && !text.empty(); ++m) {
        const size_t at = rng.below(text.size());
        switch (rng.below(3)) {
          case 0: text[at] = alphabet[rng.below(alphabet.size())]; break;
          case 1: text.erase(at, 1 + rng.below(4)); break;
          case 2: text.insert(at, 1, alphabet[rng.below(alphabet.size())]); break;
        }
      }
    } else {
      const size_t length = rng.below(96);
      for (size_t c = 0; c < length; ++c) {
        text.push_back(alphabet[rng.below(alphabet.size())]);
      }
    }
    gbx::QueryError error;
    auto query = gbx::Query::parse(text, &error);
    if (!query.valid()) continue;
    ++valid;
    size_t inspected = 0;
    for (const auto& record : trace.records) {
      (void)query.matches(trace, record);
      if (++inspected > 500) break;
    }
    gbx::run_query(trace, query, 0, trace.end_ts(), 16);
    ++evaluated;
  }
  std::fprintf(stderr, "query: %zu valid parsed, %zu evaluated\n", valid, evaluated);
  return 0;
}

int fuzz_json(int iterations) {
  Rng rng(0x150a);
  const std::string alphabet =
      "{}[],:\"\\0123456789abcdefnul truefalse-+.eE/ux";
  size_t parsed = 0;
  for (int i = 0; i < iterations; ++i) {
    std::string text;
    if (i % 3 == 0) {
      text = R"({"a":[1,2.5,true,null,"x"],"b":{"c":"\u00e9"}})";
      const size_t mutations = 1 + rng.below(6);
      for (size_t m = 0; m < mutations && !text.empty(); ++m) {
        const size_t at = rng.below(text.size());
        switch (rng.below(3)) {
          case 0: text[at] = alphabet[rng.below(alphabet.size())]; break;
          case 1: text.erase(at, 1 + rng.below(6)); break;
          case 2: text.insert(at, 1, alphabet[rng.below(alphabet.size())]); break;
        }
      }
    } else {
      const size_t length = rng.below(256);
      for (size_t c = 0; c < length; ++c) {
        text.push_back(static_cast<char>(rng.byte()));
      }
    }
    gbx::json::Value value;
    if (gbx::json::parse(text, value).ok()) {
      ++parsed;
      (void)value.find("a");
    }
  }
  std::fprintf(stderr, "json: %zu documents parsed\n", parsed);
  return 0;
}

int connect_to(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    ::close(fd);
    return -1;
  }
  struct timeval tv {};
  tv.tv_sec = 0;
  tv.tv_usec = 150000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

int fuzz_http(int iterations) {
  gbx::ui::HttpLimits limits;
  limits.io_timeout_ms = 100;
  gbx::ui::HttpServer server(limits);
  server.route("GET", "/ok", [](const gbx::ui::HttpRequest&,
                                gbx::ui::HttpResponse& response) {
    response.set_body("{\"ok\":true}");
  });
  server.route("POST", "/echo", [](const gbx::ui::HttpRequest& request,
                                   gbx::ui::HttpResponse& response) {
    gbx::json::Writer writer;
    writer.begin_object();
    writer.key_integer("len", static_cast<int64_t>(request.body.size()));
    writer.end_object();
    response.set_body(writer.str());
  });
  if (!server.listen("127.0.0.1", 0).ok()) return 1;
  std::thread thread([&server] { server.serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Rng rng(0x4a7);
  const std::string prefix = "GET /ok HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  size_t responses = 0;
  for (int i = 0; i < iterations; ++i) {
    const int fd = connect_to(server.port());
    if (fd < 0) continue;
    std::string request;
    if (i % 2 == 0) {
      request = prefix;
      const size_t length = rng.below(200);
      for (size_t c = 0; c < length; ++c) request.push_back(static_cast<char>(rng.byte()));
      request += "\r\n\r\n";
      if (i % 4 == 2) request += std::string(rng.below(64), 'x');
    } else {
      const size_t length = rng.below(512);
      for (size_t c = 0; c < length; ++c) request.push_back(static_cast<char>(rng.byte()));
    }
    gbx::ui::HttpServer::write_all(fd, request);
    char buffer[2048];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n > 0) ++responses;
    ::close(fd);
  }
  const int fd = connect_to(server.port());
  bool healthy = false;
  if (fd >= 0) {
    gbx::ui::HttpServer::write_all(fd, prefix + "\r\n");
    char buffer[2048];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    healthy = n > 0 && std::string(buffer, static_cast<size_t>(n)).find("200 OK") !=
                           std::string::npos;
    ::close(fd);
  }
  server.request_stop();
  thread.join();
  std::fprintf(stderr, "http: %zu responses, server healthy=%d\n", responses,
               healthy ? 1 : 0);
  return healthy ? 0 : 1;
}

int fuzz_roundtrip(int iterations) {
  Rng rng(0x90ad);
  size_t ok = 0;
  for (int i = 0; i < iterations; ++i) {
    gbx::Trace trace;
    gbx::TraceMeta meta;
    meta.app_name = "fuzz";
    meta.wall_start_ns = 123456789;
    trace.meta = meta;
    const size_t records = 1 + rng.below(64);
    std::vector<gbx::Field> fields;
    for (size_t r = 0; r < records; ++r) {
      fields.clear();
      const auto name = trace.strings.try_intern("key-" + std::to_string(rng.below(8)));
      const auto text = trace.strings.try_intern("value-" + std::to_string(rng.below(16)));
      if (!name || !text) return 1;
      fields.push_back({*name, gbx::Value::str(*text)});
      fields.push_back({*name, gbx::Value::integer(static_cast<int64_t>(rng.next()))});
      gbx::Record record;
      record.ts = rng.below(1'000'000);
      record.thread = static_cast<uint32_t>(rng.below(4));
      record.type = static_cast<gbx::RecordType>(rng.below(13));
      if (!trace.add_record(record, fields).ok()) return 1;
    }
    trace.finalize();
    const std::string path = temp_path("roundtrip.gbt");
    gbx::SaveOptions save;
    save.chunk_records = 1 + static_cast<uint32_t>(rng.below(8));
    save.chunk_bytes = 256 + rng.below(4096);
    if (!gbx::save_trace(path, trace, save).ok()) continue;
    gbx::Trace loaded;
    if (!gbx::load_trace(path, loaded).ok()) return 1;
    if (loaded.size() != trace.size()) return 1;
    if (!trace_invariants(loaded)) return 1;
    ++ok;
  }
  std::fprintf(stderr, "roundtrip: %zu traces verified\n", ok);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "reader";
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 200;
  try {
    if (mode == "reader") return fuzz_reader(iterations);
    if (mode == "query") return fuzz_query(iterations);
    if (mode == "json") return fuzz_json(iterations);
    if (mode == "http") return fuzz_http(iterations);
    if (mode == "roundtrip") return fuzz_roundtrip(iterations);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fuzz %s threw: %s\n", mode.c_str(), e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "fuzz %s threw an unknown exception\n", mode.c_str());
    return 1;
  }
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 2;
}
