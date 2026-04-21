#include "streaming_options.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace duckdb {

//------------------------------------------------------------------------------
// LineScanner
//------------------------------------------------------------------------------

void LineScanner::Scan(const data_t *data, size_t n) {
  switch (mode) {
  case NewLineMode::AUTO:
  case NewLineMode::LF:
    for (size_t i = 0; i < n; ++i) {
      if (data[i] == '\n')
        ++count;
    }
    break;
  case NewLineMode::CR:
    for (size_t i = 0; i < n; ++i) {
      if (data[i] == '\r')
        ++count;
    }
    break;
  case NewLineMode::CRLF: {
    uint8_t prev = have_last ? last_byte : 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = data[i];
      if (b == '\n' && prev == '\r')
        ++count;
      prev = b;
    }
    break;
  }
  }
  if (n > 0) {
    last_byte = data[n - 1];
    have_last = true;
  }
}

//------------------------------------------------------------------------------
// Option value decoders
//------------------------------------------------------------------------------

static string DecodeOptionValue(const string &raw, const string &key,
                                const string &full_url) {
  string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= raw.size()) {
      throw IOException(
          "Malformed escape sequence in stream option '%s=%s' in URL "
          "'%s'. Only \\r and \\n are recognized.",
          key, raw, full_url);
    }
    char next = raw[++i];
    if (next == 'r') {
      out.push_back('\r');
    } else if (next == 'n') {
      out.push_back('\n');
    } else {
      throw IOException(
          "Malformed escape sequence in stream option '%s=%s' in URL "
          "'%s'. Only \\r and \\n are recognized.",
          key, raw, full_url);
    }
  }
  return out;
}

static idx_t ParseSizeWithSuffix(const string &raw, const string &full_url) {
  if (raw.empty()) {
    throw IOException(
        "Invalid max_bytes value '%s' in URL '%s'. Expected integer with "
        "optional KB/MB/GB suffix.",
        raw, full_url);
  }
  size_t digits_end = 0;
  while (digits_end < raw.size() &&
         std::isdigit(static_cast<unsigned char>(raw[digits_end]))) {
    ++digits_end;
  }
  if (digits_end == 0) {
    throw IOException(
        "Invalid max_bytes value '%s' in URL '%s'. Expected integer with "
        "optional KB/MB/GB suffix.",
        raw, full_url);
  }
  string digits = raw.substr(0, digits_end);
  string suffix = raw.substr(digits_end);
  for (auto &c : suffix) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  idx_t multiplier = 1;
  if (suffix.empty()) {
    multiplier = 1;
  } else if (suffix == "KB") {
    multiplier = 1024ULL;
  } else if (suffix == "MB") {
    multiplier = 1024ULL * 1024;
  } else if (suffix == "GB") {
    multiplier = 1024ULL * 1024 * 1024;
  } else {
    throw IOException(
        "Invalid max_bytes value '%s' in URL '%s'. Expected integer with "
        "optional KB/MB/GB suffix.",
        raw, full_url);
  }
  idx_t base = 0;
  for (char c : digits) {
    idx_t d = static_cast<idx_t>(c - '0');
    if (base > (std::numeric_limits<idx_t>::max() - d) / 10) {
      throw IOException(
          "Invalid max_bytes value '%s' in URL '%s': numeric overflow.", raw,
          full_url);
    }
    base = base * 10 + d;
  }
  if (multiplier > 1 && base > std::numeric_limits<idx_t>::max() / multiplier) {
    throw IOException(
        "Invalid max_bytes value '%s' in URL '%s': numeric overflow.", raw,
        full_url);
  }
  return base * multiplier;
}

//------------------------------------------------------------------------------
// Public parsers
//------------------------------------------------------------------------------

void ParseStreamingOptions(const string &segment, StreamingOptions &out,
                           const string &full_url) {
  if (segment.empty()) {
    return;
  }
  auto is_space = [](char c) { return c == ' ' || c == '\t'; };

  size_t pos = 0;
  while (pos < segment.size()) {
    size_t comma = segment.find(',', pos);
    string pair = (comma == string::npos) ? segment.substr(pos)
                                          : segment.substr(pos, comma - pos);
    pos = (comma == string::npos) ? segment.size() : comma + 1;
    while (!pair.empty() && is_space(pair.front()))
      pair.erase(pair.begin());
    while (!pair.empty() && is_space(pair.back()))
      pair.pop_back();
    if (pair.empty())
      continue;
    size_t eq = pair.find('=');
    if (eq == string::npos) {
      throw IOException(
          "Malformed zip-stream option '%s' in URL '%s'. Expected key=value.",
          pair, full_url);
    }
    string key = pair.substr(0, eq);
    string raw_value = pair.substr(eq + 1);
    while (!key.empty() && is_space(key.back()))
      key.pop_back();
    while (!raw_value.empty() && is_space(raw_value.front()))
      raw_value.erase(raw_value.begin());
    while (!raw_value.empty() && is_space(raw_value.back()))
      raw_value.pop_back();

    if (key == "lines") {
      idx_t n = 0;
      for (char c : raw_value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          throw IOException(
              "Invalid lines value '%s' in URL '%s'. Expected a non-negative "
              "integer.",
              raw_value, full_url);
        }
        idx_t d = static_cast<idx_t>(c - '0');
        if (n > (std::numeric_limits<idx_t>::max() - d) / 10) {
          throw IOException(
              "Invalid lines value '%s' in URL '%s': numeric overflow.",
              raw_value, full_url);
        }
        n = n * 10 + d;
      }
      out.lines = n;
    } else if (key == "new_line") {
      string decoded = DecodeOptionValue(raw_value, key, full_url);
      if (decoded == "\n") {
        out.new_line = NewLineMode::LF;
      } else if (decoded == "\r") {
        out.new_line = NewLineMode::CR;
      } else if (decoded == "\r\n") {
        out.new_line = NewLineMode::CRLF;
      } else {
        throw IOException(
            "Invalid new_line value '%s' in URL '%s'. Must be one of: "
            "\\n, \\r, \\r\\n.",
            raw_value, full_url);
      }
    } else if (key == "max_bytes") {
      out.max_bytes = ParseSizeWithSuffix(raw_value, full_url);
    } else {
      throw IOException("Unknown stream option '%s' in URL '%s'. Valid "
                        "keys: lines, new_line, max_bytes.",
                        key, full_url);
    }
  }
}

void ParseBracketedOptions(const string &body, const string &full_url,
                           StreamingOptions &out_options,
                           string &out_options_literal,
                           string &out_inner_body) {
  out_options = StreamingOptions();
  out_options_literal.clear();

  if (body.empty() || body[0] != '[') {
    out_inner_body = body;
    return;
  }

  size_t close = body.find(']', 1);
  if (close == string::npos) {
    throw IOException("Unterminated '[' in stream URL '%s'.", full_url);
  }

  string segment = body.substr(1, close - 1);
  ParseStreamingOptions(segment, out_options, full_url);
  out_options_literal = body.substr(0, close + 1);

  // Skip a single separator slash so callers can write
  // `<scheme>://[...]/path` consistently with the no-brackets form.
  size_t inner_start = close + 1;
  if (inner_start < body.size() && body[inner_start] == '/') {
    inner_start += 1;
  }
  out_inner_body = body.substr(inner_start);
}

} // namespace duckdb
