// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include "glassbox/json.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "test_framework.hpp"

TEST(json_writer_basic_document) {
  gbx::json::Writer writer;
  writer.begin_object();
  writer.key_string("name", "glassbox");
  writer.key_integer("version", 1);
  writer.key_number("ratio", 0.5);
  writer.key_boolean("ok", true);
  writer.key_null("nothing");
  writer.key("items");
  writer.begin_array();
  writer.integer(1);
  writer.integer(2);
  writer.end_array();
  writer.end_object();
  const auto text = writer.str();
  CHECK(text.find("\"name\":\"glassbox\"") != std::string::npos);
  CHECK(text.find("\"ok\":true") != std::string::npos);
  CHECK(text.find("\"nothing\":null") != std::string::npos);
  CHECK(text.find("\"items\":[1,2]") != std::string::npos);

  gbx::json::Value value;
  auto status = gbx::json::parse(text, value);
  CHECK_MSG(status.ok(), status.describe());
  CHECK(value.is_object());
  CHECK_EQ(value.find("name")->as_string(), std::string("glassbox"));
  CHECK_EQ(value.find("version")->as_number(), 1.0);
  CHECK_EQ(value.find("items")->array.size(), size_t{2});
}

TEST(json_writer_escapes_and_sanitizes) {
  gbx::json::Writer writer;
  writer.begin_array();
  writer.value("quote\" backslash\\ newline\n tab\t control\x01");
  writer.value("bad\xff\xfeutf8");
  writer.value("emoji \xf0\x9f\x98\x80");
  writer.end_array();
  gbx::json::Value value;
  auto status = gbx::json::parse(writer.str(), value);
  CHECK_MSG(status.ok(), status.describe());
  CHECK_EQ(value.array[0].string,
           std::string("quote\" backslash\\ newline\n tab\t control\x01"));
  CHECK_EQ(value.array[1].string,
           std::string("bad\xef\xbf\xbd\xef\xbf\xbdutf8"));
  CHECK_EQ(value.array[2].string, std::string("emoji \xf0\x9f\x98\x80"));
}

TEST(json_writer_non_finite_and_large_integers) {
  gbx::json::Writer writer;
  writer.begin_array();
  writer.number(std::nan(""));
  writer.number(std::numeric_limits<double>::infinity());
  writer.integer(9007199254740993ll);
  writer.integer(-9007199254740993ll);
  writer.end_array();
  const auto text = writer.str();
  CHECK_EQ(text, std::string("[null,null,\"9007199254740993\",\"-9007199254740993\"]"));
}

TEST(json_parser_accepts_escapes_and_surrogates) {
  gbx::json::Value value;
  const std::string text =
      "{\"a\":\"line\\nbreak\",\"b\":\"\\u00e9\",\"c\":\"\\ud83d\\ude00\","
      "\"n\":-1.5e2,\"t\":true,\"z\":null,\"arr\":[1,{\"x\":2}]}";
  auto status = gbx::json::parse(text, value);
  CHECK_MSG(status.ok(), status.describe());
  CHECK_EQ(value.find("a")->as_string(), std::string("line\nbreak"));
  CHECK_EQ(value.find("b")->as_string(), std::string("\xc3\xa9"));
  CHECK_EQ(value.find("c")->as_string(), std::string("\xf0\x9f\x98\x80"));
  CHECK_EQ(value.find("n")->as_number(), -150.0);
  CHECK(value.find("t")->as_boolean());
  CHECK(value.find("z")->is_null());
  CHECK_EQ(value.find("arr")->array.size(), size_t{2});
  CHECK_EQ(value.find("arr")->array[1].find("x")->as_number(), 2.0);
}

TEST(json_parser_rejects_malformed) {
  const char* invalid[] = {
      "",         "{",        "[]x",      "{\"a\":}", "{\"a\" 1}",
      "[1,]",     "{\"a\":1,}", "[01]",    "[1.]",     "[1e]",
      "\"abc",    "truex",    "{\"a\":1 \"b\":2}",     "[1 2]",
      "{\"a\":\"\\ud800\"}", "{\"a\":\"\\udc00\"}",     "nan",
  };
  for (const char* text : invalid) {
    gbx::json::Value value;
    auto status = gbx::json::parse(text, value);
    CHECK_MSG(!status.ok(), text);
    if (!status.ok()) CHECK(status.code() == gbx::ErrorCode::Corrupt);
  }
}

TEST(json_parser_limits) {
  gbx::json::ParseLimits limits;
  limits.max_depth = 4;
  limits.max_string = 4;
  limits.max_array = 2;
  limits.max_object = 2;
  gbx::json::Value value;
  CHECK(!gbx::json::parse("[[[[[1]]]]]", value, limits).ok());
  CHECK(!gbx::json::parse("\"too long\"", value, limits).ok());
  CHECK(!gbx::json::parse("[1,2,3]", value, limits).ok());
  CHECK(!gbx::json::parse("{\"a\":1,\"b\":2,\"c\":3}", value, limits).ok());
  limits.max_bytes = 4;
  CHECK(!gbx::json::parse("[1,2]", value, limits).ok());
}

TEST(json_quote_helper) {
  CHECK_EQ(gbx::json::quote("a\"b"), std::string("\"a\\\"b\""));
  gbx::json::Value value;
  auto status = gbx::json::parse(gbx::json::quote("x\ny"), value);
  CHECK(status.ok());
  CHECK_EQ(value.string, std::string("x\ny"));
}
