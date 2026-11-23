// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace gbx {

/// The kind of a trace record. The type determines which well-known fields
/// carry meaning (see docs/trace-format.md); the encoding itself is generic.
enum class RecordType : uint8_t {
  Meta = 0,
  ThreadName = 1,
  SpanBegin = 2,
  SpanEnd = 3,
  Log = 4,
  Counter = 5,
  Gauge = 6,
  Alloc = 7,
  Free = 8,
  Snapshot = 9,
  Sample = 10,
  Marker = 11,
  Custom = 12,
};

/// Severity of a log record. Values are stable in the file format.
enum class LogLevel : uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Fatal = 5,
};

std::string_view to_string(RecordType type);
std::string_view to_string(LogLevel level);

/// Parses a type or level name; returns false for unknown names.
bool parse_record_type(std::string_view text, RecordType& out);
bool parse_log_level(std::string_view text, LogLevel& out);

/// Value tag stored with each field.
enum class ValueType : uint8_t {
  Int = 1,
  Float = 2,
  String = 3,
  Bool = 4,
};

/// A dynamically typed field value. `s` is an index into the trace string
/// table; it is only meaningful when `type == String` (or for bools).
struct Value {
  ValueType type = ValueType::Int;
  int64_t i = 0;
  double f = 0.0;
  uint32_t s = 0;

  static Value integer(int64_t v) { return {ValueType::Int, v, 0.0, 0}; }
  static Value real(double v) { return {ValueType::Float, 0, v, 0}; }
  static Value str(uint32_t id) { return {ValueType::String, 0, 0.0, id}; }
  static Value boolean(bool v) {
    return {ValueType::Bool, v ? 1 : 0, 0.0, 0};
  }
  /// Encodes a pointer exactly, including addresses with the high bit set.
  static Value pointer(const void* p) {
    int64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(p), "pointer size");
    std::memcpy(&bits, &p, sizeof(bits));
    return integer(bits);
  }
  /// Returns the raw pointer bits as an unsigned integer.
  uint64_t pointer_bits() const {
    uint64_t bits = static_cast<uint64_t>(i);
    return bits;
  }

  double as_double() const {
    return type == ValueType::Float ? f : static_cast<double>(i);
  }
  int64_t as_int() const {
    return type == ValueType::Float ? static_cast<int64_t>(f) : i;
  }
  bool as_bool() const {
    return type == ValueType::Float ? (f != 0.0)
                                    : (type == ValueType::String ? s != 0
                                                                 : i != 0);
  }
  bool is_numeric() const {
    return type == ValueType::Int || type == ValueType::Float ||
           type == ValueType::Bool;
  }
};

/// A key/value pair attached to a record. Keys are interned strings.
struct Field {
  uint32_t key = 0;
  Value value;
};

/// A single timestamped observation in a trace.
///
/// `ts` is nanoseconds since the start of the recording. `thread` is a dense
/// thread slot (0 is reserved for "unknown"). `span` is the innermost span
/// active on that thread when the record was emitted, or 0 when none was.
/// Fields live in the owning trace's flat field array.
struct Record {
  uint64_t ts = 0;
  uint32_t thread = 0;
  uint32_t span = 0;
  uint32_t first_field = 0;
  uint16_t field_count = 0;
  RecordType type = RecordType::Marker;
  uint8_t flags = 0;

  std::span<const Field> field_span(const std::vector<Field>& all) const {
    const size_t begin = first_field;
    if (begin > all.size()) return {};
    const size_t count =
        field_count < all.size() - begin ? field_count : all.size() - begin;
    return {all.data() + begin, count};
  }
};

/// Maximum number of fields a single record may carry.
inline constexpr uint16_t kMaxRecordFields = 64;

/// Well-known field keys. Records may carry arbitrary additional keys, but
/// these names have defined meaning per record type.
namespace keys {
inline constexpr std::string_view kName = "name";
inline constexpr std::string_view kKind = "kind";
inline constexpr std::string_view kParent = "parent";
inline constexpr std::string_view kId = "id";
inline constexpr std::string_view kMsg = "msg";
inline constexpr std::string_view kLevel = "level";
inline constexpr std::string_view kCategory = "category";
inline constexpr std::string_view kFile = "file";
inline constexpr std::string_view kLine = "line";
inline constexpr std::string_view kSize = "size";
inline constexpr std::string_view kPtr = "ptr";
inline constexpr std::string_view kSite = "site";
inline constexpr std::string_view kClass = "class";
inline constexpr std::string_view kOwner = "owner";
inline constexpr std::string_view kValue = "value";
inline constexpr std::string_view kUnit = "unit";
inline constexpr std::string_view kFrames = "frames";
inline constexpr std::string_view kWeight = "weight";
inline constexpr std::string_view kPc = "pc";
inline constexpr std::string_view kLabel = "label";
inline constexpr std::string_view kAppName = "app";
inline constexpr std::string_view kAppVersion = "version";
inline constexpr std::string_view kExe = "exe";
inline constexpr std::string_view kArch = "arch";
inline constexpr std::string_view kOs = "os";
inline constexpr std::string_view kPid = "pid";
inline constexpr std::string_view kWallNs = "wall_ns";
}  // namespace keys

/// Frame separator used inside the `frames` string of a sample record.
inline constexpr char kFrameSeparator = '\x1f';

/// Formats a value for diagnostics. String values render as `#<id>`.
std::string format_value(const Value& value);

}  // namespace gbx
