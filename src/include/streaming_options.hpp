#pragma once

#include "duckdb/common/file_system.hpp"

namespace duckdb {

enum class NewLineMode { AUTO, LF, CR, CRLF };

struct StreamingOptions {
  idx_t lines = 20480;
  NewLineMode new_line = NewLineMode::AUTO;
  idx_t max_bytes = 64ULL * 1024 * 1024;
};

// Scans buffers for line-delimiter occurrences to drive prefix inflation's
// stopping condition. Keeps one byte of carry across chunk boundaries for
// the \r\n case.
struct LineScanner {
  NewLineMode mode;
  uint8_t last_byte = 0;
  bool have_last = false;
  idx_t count = 0;

  explicit LineScanner(NewLineMode mode_p) : mode(mode_p) {}
  void Scan(const data_t *data, size_t n);
};

// Parses the contents between `[` and `]` of a bracketed options segment.
// `full_url` is used only for error messages.
void ParseStreamingOptions(const string &segment, StreamingOptions &out,
                           const string &full_url);

// Given a `body` that starts immediately after a scheme prefix (e.g.,
// everything in `zip-stream://...` after the initial 13 characters), detect
// an optional bracketed options segment at the start. On return:
//  - `out_options` holds parsed options (or defaults when no brackets)
//  - `out_options_literal` is the verbatim bracket segment including the
//    brackets (e.g., "[max_bytes=10]"), or empty if absent; preserved for
//    round-tripping through Glob
//  - `out_inner_body` is the remainder of `body` past the brackets and a
//    single optional separator slash (so callers can feed it straight to
//    path-splitting helpers)
// Throws IOException for an unterminated `[`.
void ParseBracketedOptions(const string &body, const string &full_url,
                           StreamingOptions &out_options,
                           string &out_options_literal, string &out_inner_body);

} // namespace duckdb
