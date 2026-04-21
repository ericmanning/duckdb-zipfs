#include "zip_file_system.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>

namespace duckdb {

auto const ZIP_SEPARATOR = "/";
static constexpr const char *ZIP_SCHEME = "zip://";
static constexpr size_t ZIP_SCHEME_LEN = 6;
static constexpr const char *STREAM_SCHEME = "zip-stream://";
static constexpr size_t STREAM_SCHEME_LEN = 13;

// TODO: Something is incorrect about the type in make_uniq_array<...,
// std::default_delete<DATA_TYPE>, ...>
template <class DATA_TYPE>
inline unique_ptr<DATA_TYPE[], std::default_delete<DATA_TYPE[]>, true>
make_uniq_array2(size_t n) // NOLINT: mimic std style
{
  return unique_ptr<DATA_TYPE[], std::default_delete<DATA_TYPE[]>, true>(
      new DATA_TYPE[n]());
}

//------------------------------------------------------------------------------
// Scheme detection
//------------------------------------------------------------------------------

struct ParsedZipPath {
  bool streaming;    // false for zip://, true for zip-stream://
  string inner_body; // the archive path + entry, past scheme and brackets
  // Byte-for-byte options segment (including brackets), preserved verbatim
  // for round-tripping through Glob. Empty when no bracketed options.
  string options_literal;
  StreamingOptions options;
};

// Dispatches on the zip:// or zip-stream:// scheme prefix and, for
// zip-stream://, defers bracketed-options parsing to the shared helper.
static bool DetectSchemeAndOptions(const string &fpath, ParsedZipPath &out) {
  if (fpath.size() > ZIP_SCHEME_LEN &&
      fpath.compare(0, ZIP_SCHEME_LEN, ZIP_SCHEME) == 0) {
    out.streaming = false;
    out.options_literal.clear();
    out.options = StreamingOptions();
    out.inner_body = fpath.substr(ZIP_SCHEME_LEN);
    return true;
  }
  if (fpath.size() > STREAM_SCHEME_LEN &&
      fpath.compare(0, STREAM_SCHEME_LEN, STREAM_SCHEME) == 0) {
    out.streaming = true;
    ParseBracketedOptions(fpath.substr(STREAM_SCHEME_LEN), fpath, out.options,
                          out.options_literal, out.inner_body);
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
// Zip Utilities
//------------------------------------------------------------------------------

// Split a zip path into the path to the archive and the path within the archive
static pair<string, string> SplitArchivePath(const string &path,
                                             ClientContext &context) {
  Value zipfs_split_value = Value(LogicalType::VARCHAR);
  context.TryGetCurrentSetting("zipfs_split", zipfs_split_value);

  if (!zipfs_split_value.IsNull()) {
    auto zipfs_split_str = zipfs_split_value.GetValue<string>();

    const auto zip_path =
        std::search(path.begin(), path.end(), zipfs_split_str.begin(),
                    zipfs_split_str.end());

    const auto suffix_found = zip_path != path.end();
    const auto suffix_path =
        suffix_found
            ? zip_path + UnsafeNumericCast<int64_t>(zipfs_split_str.size())
            : zip_path;

    if (suffix_path == path.end()) {
      // Glob entire zip file by default
      return {string(path.begin(),
                     path.end() - (suffix_found ? zipfs_split_str.size() : 0)),
              "**"};
    }

    // If there is a slash after the last .zip, we need to remove everything
    // after that
    auto archive_path =
        string(path.begin(),
               suffix_path - (suffix_found ? zipfs_split_str.size() : 0));
    auto file_path =
        string(suffix_path + (*suffix_path == '/' ? 1 : 0), path.end());
    return {archive_path, file_path};
  } else {
    Value zipfs_extension_value = ".zip";
    context.TryGetCurrentSetting("zipfs_extension", zipfs_extension_value);

    auto zipfs_extension_str = zipfs_extension_value.GetValue<string>();

    const auto zip_path =
        std::search(path.begin(), path.end(), zipfs_extension_str.begin(),
                    zipfs_extension_str.end());

    if (zip_path == path.end()) {
      throw IOException("Could not find a '%s' archive to open in: '%s'",
                        zipfs_extension_str.c_str(), path);
    }

    const auto suffix_path =
        zip_path + UnsafeNumericCast<int64_t>(zipfs_extension_str.size());

    if (suffix_path == path.end()) {
      // Glob entire zip file by default
      return {path, "**"};
    }

    if (*suffix_path == '/') {
      // If there is a slash after the last .zip, we need to remove everything
      // after that
      auto archive_path = string(path.begin(), suffix_path);
      auto file_path = string(suffix_path + 1, path.end());
      return {archive_path, file_path};
    }

    throw IOException(
        "Could not find valid path within '%s' archive to open in: '%s'",
        zipfs_extension_str.c_str(), path);
  }
}

//------------------------------------------------------------------------------
// Miniz I/O callback (shared)
//------------------------------------------------------------------------------

size_t FileSystemZipReadFunc(void *pOpaque, mz_uint64 file_ofs, void *pBuf,
                             size_t n) {
  FileHandle *handle = (FileHandle *)pOpaque;
  handle->Seek(UnsafeNumericCast<idx_t>(file_ofs));
  return UnsafeNumericCast<size_t>(handle->Read(pBuf, n));
}

//------------------------------------------------------------------------------
// Zip File Handle
//------------------------------------------------------------------------------

void ZipFileHandle::Close() { inner_handle->Close(); }

//------------------------------------------------------------------------------
// Zip File System (zip://)
//------------------------------------------------------------------------------

bool ZipFileSystem::CanHandleFile(const string &fpath) {
  // Only the plain `zip://` scheme. `zip-stream://` is handled by
  // StreamingZipFileSystem below, and never collides here because its prefix
  // does not match the 6-byte `zip://` compare.
  return fpath.size() > ZIP_SCHEME_LEN &&
         fpath.compare(0, ZIP_SCHEME_LEN, ZIP_SCHEME) == 0;
}

unique_ptr<FileHandle>
ZipFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                        optional_ptr<FileOpener> opener) {
  if (!flags.OpenForReading() || flags.OpenForWriting()) {
    throw IOException("Zip file system can only open for reading");
  }

  // Get the path to the zip file
  auto context = opener->TryGetClientContext();
  const auto paths = SplitArchivePath(path.substr(ZIP_SCHEME_LEN), *context);
  const auto &zip_path = paths.first;
  const auto &file_path = paths.second;

  // Now we need to find the file within the zip file and return out file handle
  auto &fs = FileSystem::GetFileSystem(*context);
  auto handle = fs.OpenFile(zip_path, flags);
  if (!handle) {
    throw IOException("Failed to open file: %s", zip_path);
  }

  if (file_path.empty()) {
    return handle;
  }

  auto normalized_file_path = StringUtil::Replace(
      file_path, fs.PathSeparator(file_path), ZIP_SEPARATOR);

  if (!handle->CanSeek()) {
    // TODO: Buffer?
    throw IOException("Cannot seek");
  }

  idx_t size = handle->GetFileSize();

  mz_zip_archive zip;
  mz_zip_zero_struct(&zip);
  zip.m_pRead = &FileSystemZipReadFunc;
  zip.m_pIO_opaque = handle.get();
  try {
    mz_uint zip_flags = 0;

    if (!mz_zip_reader_init(&zip, size, zip_flags)) {
      throw IOException("Could not open as zip file: %s",
                        mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    }

    mz_uint file_index = 0;
    auto locate_failed =
        mz_zip_reader_locate_file_v2(&zip, normalized_file_path.c_str(),
                                     nullptr, 0, &file_index) == MZ_FALSE;
    if (locate_failed) {
      throw IOException("Failed to find file: %s", normalized_file_path);
    }

    mz_zip_archive_file_stat file_stat = {0};
    auto stat_failed =
        mz_zip_reader_file_stat(&zip, file_index, &file_stat) == MZ_FALSE;

    if (stat_failed) {
      throw IOException("Problem stat-ing file within archive: %s",
                        mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    }
    if ((file_stat.m_method) && (file_stat.m_method != MZ_DEFLATED)) {
      throw IOException("Unknown compression method");
    }

    auto read_buf = make_uniq_array2<data_t>(file_stat.m_uncomp_size);
    mz_zip_reader_extract_file_to_mem(
        &zip, file_stat.m_filename, read_buf.get(), file_stat.m_uncomp_size, 0);

    auto zip_file_handle = make_uniq<ZipFileHandle>(
        *this, path, flags, std::move(handle), file_stat, std::move(read_buf));

    mz_zip_reader_end(&zip);

    return zip_file_handle;
  } catch (Exception &ex) {
    mz_zip_reader_end(&zip);
    throw;
  }
}

void ZipFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes,
                         idx_t location) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  auto remaining_bytes = t_handle.file_stat.m_uncomp_size - location;
  auto to_read = MinValue(UnsafeNumericCast<idx_t>(nr_bytes), remaining_bytes);
  memcpy(buffer, t_handle.data.get() + location, to_read);
}

int64_t ZipFileSystem::Read(FileHandle &handle, void *buffer,
                            int64_t nr_bytes) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  auto position = t_handle.seek_offset;
  auto remaining_bytes = t_handle.file_stat.m_uncomp_size - position;
  auto to_read = MinValue(UnsafeNumericCast<idx_t>(nr_bytes), remaining_bytes);
  memcpy(buffer, t_handle.data.get() + position, to_read);
  t_handle.seek_offset += to_read;
  return to_read;
}

int64_t ZipFileSystem::GetFileSize(FileHandle &handle) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  return UnsafeNumericCast<int64_t>(t_handle.file_stat.m_uncomp_size);
}

void ZipFileSystem::Seek(FileHandle &handle, idx_t location) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  t_handle.seek_offset = location;
}

void ZipFileSystem::Reset(FileHandle &handle) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  t_handle.seek_offset = 0;
}

idx_t ZipFileSystem::SeekPosition(FileHandle &handle) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  return t_handle.seek_offset;
}

bool ZipFileSystem::CanSeek() { return true; }

timestamp_t ZipFileSystem::GetLastModifiedTime(FileHandle &handle) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  auto &inner_handle = *t_handle.inner_handle;
  return inner_handle.file_system.GetLastModifiedTime(inner_handle);
}

FileType ZipFileSystem::GetFileType(FileHandle &handle) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  auto &inner_handle = *t_handle.inner_handle;
  return inner_handle.file_system.GetFileType(inner_handle);
}

bool ZipFileSystem::OnDiskFile(FileHandle &handle) {
  auto &t_handle = handle.Cast<ZipFileHandle>();
  return t_handle.inner_handle->OnDiskFile();
}

//------------------------------------------------------------------------------
// Shared Glob body (used by both ZipFileSystem and StreamingZipFileSystem)
//------------------------------------------------------------------------------

static vector<OpenFileInfo> GlobZip(FileOpener *opener,
                                    const string &scheme_literal,
                                    const string &options_literal,
                                    const string &inner_body) {
  auto context = opener->TryGetClientContext();
  auto &fs = FileSystem::GetFileSystem(*context);
  const auto parts = SplitArchivePath(inner_body, *context);
  auto &zip_path = parts.first;
  const auto has_glob = FileSystem::HasGlob(zip_path);
  auto &file_path = parts.second;

  // Get matching zip files
  vector<OpenFileInfo> matching_zips;
  if (has_glob) {
    matching_zips = fs.GlobFiles(zip_path, FileGlobOptions::DISALLOW_EMPTY);
  } else {
    // Normally, GlobFiles would be safe. However, when
    // there is no glob, we don't call it because it can mangle https:// URLs
    // (converting slashes into backslashes.)
    matching_zips = {OpenFileInfo(zip_path)};
  }

  Value zipfs_split_value = Value(LogicalType::VARCHAR);
  context->TryGetCurrentSetting("zipfs_split", zipfs_split_value);

  auto extension =
      !zipfs_split_value.IsNull() ? zipfs_split_value.GetValue<string>() : "";

  // Preserve the bracketed options segment byte-for-byte so downstream
  // consumers (e.g., the file system that receives the rewritten path back via
  // OpenFile) parse the same options we just parsed.
  const string prefix =
      scheme_literal +
      (options_literal.empty() ? string() : options_literal + "/");

  vector<OpenFileInfo> result;
  for (const auto &curr_zip : matching_zips) {
    if (!FileSystem::HasGlob(file_path)) {
      // No glob pattern in the file path, just return the file path
      result.push_back(prefix + curr_zip.path + extension + ZIP_SEPARATOR +
                       file_path);
      continue;
    }

    auto pattern_parts = StringUtil::Split(file_path, ZIP_SEPARATOR);
    // TODO: We may want to detect globbing into a nested zip file and reject.

    // Given the path to the zip file, open it
    auto archive_handle = fs.OpenFile(curr_zip, FileFlags::FILE_FLAGS_READ);
    if (!archive_handle) {
      continue; // Skip invalid zip files
    }
    if (!archive_handle->CanSeek()) {
      continue; // Skip unseekable files
    }

    idx_t size = archive_handle->GetFileSize();

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pRead = &FileSystemZipReadFunc;
    zip.m_pIO_opaque = archive_handle.get();

    string zip_filename;
    const size_t MAX_FILENAME_LEN = 65536; // = 2**16
    zip_filename.reserve(1024);
    try {
      mz_uint flags = 0;

      if (!mz_zip_reader_init(&zip, size, flags)) {
        throw IOException("Could not open as zip file: %s",
                          mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
      }

      mz_uint i, files;

      files = mz_zip_reader_get_num_files(&zip);

      for (i = 0; i < files; i++) {
        mz_zip_clear_last_error(&zip);

        if (mz_zip_reader_is_file_a_directory(&zip, i))
          continue;

        mz_zip_validate_file(&zip, i, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY);

        if (mz_zip_reader_is_file_encrypted(&zip, i))
          continue;

        mz_zip_clear_last_error(&zip);

        mz_uint filename_size = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        // NOTE: filename_size already contains +1 for the leading \0
        // Double filename capacity/length until it's enough or larger than
        // 2**16, where 2**16 should be the max filename length in zip files.
        if (filename_size > zip_filename.capacity()) {
          size_t new_capacity =
              zip_filename.capacity() > 0 ? zip_filename.capacity() : 1;
          while (new_capacity < filename_size) {
            new_capacity *= 2;
            if (new_capacity > MAX_FILENAME_LEN) {
              throw IOException("Filename too long");
            }
          }
          zip_filename.reserve(new_capacity);
        }
        zip_filename.resize(filename_size - 1);
        mz_zip_reader_get_filename(&zip, i, &zip_filename[0], filename_size);

        if (auto err = mz_zip_get_last_error(&zip)) {
          throw IOException("Problem getting filename: %s",
                            mz_zip_get_error_string(err));
        }

        auto entry_parts = StringUtil::Split(zip_filename, ZIP_SEPARATOR);

        if (entry_parts.size() < pattern_parts.size()) {
          // This entry is not deep enough to match the pattern
          continue;
        }

        // Check if the pattern matches the entry
        bool match = true;
        for (idx_t i = 0; i < pattern_parts.size(); i++) {
          const auto &pp = pattern_parts[i];
          const auto &ep = entry_parts[i];

          if (pp == "**") {
            // We only allow crawl's to be at the end of the pattern
            if (i != pattern_parts.size() - 1) {
              throw NotImplementedException(
                  "Recursive globs are only supported at the end of zip file "
                  "path patterns");
            }
            // Otherwise, everything else is a match
            match = true;
            break;
          }

          if (!duckdb::Glob(ep.c_str(), ep.size(), pp.c_str(), pp.size())) {
            // Not a match
            match = false;
            break;
          }

          if (i == pattern_parts.size() - 1 &&
              entry_parts.size() > pattern_parts.size()) {
            // If the entry is deeper than the pattern (and we havent hit a **),
            // then it is not a match
            match = false;
            break;
          }
        }

        if (match) {
          auto entry_path =
              prefix + curr_zip.path + extension + ZIP_SEPARATOR + zip_filename;
          // Cache here???
          result.push_back(entry_path);
        }
      }

      mz_zip_reader_end(&zip);
    } catch (Exception &ex) {
      mz_zip_reader_end(&zip);
      throw;
    }
  }

  return result;
}

vector<OpenFileInfo> ZipFileSystem::Glob(const string &path,
                                         FileOpener *opener) {
  return GlobZip(opener, ZIP_SCHEME, /*options_literal=*/"",
                 path.substr(ZIP_SCHEME_LEN));
}

//------------------------------------------------------------------------------
// Shared FileExists body
//------------------------------------------------------------------------------

static bool FileExistsZip(optional_ptr<FileOpener> opener,
                          const string &inner_body) {
  auto context = opener->TryGetClientContext();
  const auto parts = SplitArchivePath(inner_body, *context);
  auto &zip_path = parts.first;
  auto &file_path = parts.second;

  auto &fs = FileSystem::GetFileSystem(*context);
  // Do not pass opener here, as it will crash later.
  if (!fs.FileExists(zip_path)) {
    return false;
  }

  auto normalized_file_path = StringUtil::Replace(
      file_path, fs.PathSeparator(file_path), ZIP_SEPARATOR);

  auto handle = fs.OpenFile(zip_path, FileOpenFlags::FILE_FLAGS_READ);
  if (!handle) {
    return false;
  }

  if (!handle->CanSeek()) {
    // TODO: Buffer?
    return false;
  }

  idx_t size = handle->GetFileSize();

  mz_zip_archive zip;
  mz_zip_zero_struct(&zip);
  zip.m_pRead = &FileSystemZipReadFunc;
  zip.m_pIO_opaque = handle.get();
  try {
    mz_uint zip_flags = 0;

    if (!mz_zip_reader_init(&zip, size, zip_flags)) {
      return false;
    }

    mz_uint file_index = 0;
    auto locate_failed =
        mz_zip_reader_locate_file_v2(&zip, normalized_file_path.c_str(),
                                     nullptr, 0, &file_index) == MZ_FALSE;
    if (locate_failed) {
      return false;
    }

    mz_zip_archive_file_stat file_stat = {0};
    auto stat_failed =
        mz_zip_reader_file_stat(&zip, file_index, &file_stat) == MZ_FALSE;

    if (stat_failed) {
      return false;
    }
    if ((file_stat.m_method) && (file_stat.m_method != MZ_DEFLATED)) {
      return false;
    }

    mz_zip_reader_end(&zip);

    return true;
  } catch (Exception &ex) {
    mz_zip_reader_end(&zip);
    throw;
  }
}

bool ZipFileSystem::FileExists(const string &filename,
                               optional_ptr<FileOpener> opener) {
  return FileExistsZip(opener, filename.substr(ZIP_SCHEME_LEN));
}

//------------------------------------------------------------------------------
// Streaming Zip File Handle
//------------------------------------------------------------------------------

void StreamingZipFileHandle::FreeStream() {
  if (stream_freed) {
    return;
  }
  if (iter != nullptr) {
    mz_zip_reader_extract_iter_free(iter);
    iter = nullptr;
  }
  if (zip) {
    mz_zip_reader_end(zip.get());
  }
  stream_freed = true;
}

StreamingZipFileHandle::~StreamingZipFileHandle() { FreeStream(); }

void StreamingZipFileHandle::Close() {
  FreeStream();
  if (inner_handle) {
    inner_handle->Close();
  }
}

//------------------------------------------------------------------------------
// Streaming Zip File System (zip-stream://)
//------------------------------------------------------------------------------

bool StreamingZipFileSystem::CanHandleFile(const string &fpath) {
  return fpath.size() > STREAM_SCHEME_LEN &&
         fpath.compare(0, STREAM_SCHEME_LEN, STREAM_SCHEME) == 0;
}

namespace {
// Context used only while building a streaming handle or its degenerate
// fallback. Bundles the raw path strings and the inner FileHandle so the
// OpenFile site can hand ownership off cleanly.
struct PrefixInflateResult {
  unique_ptr<data_t[]> prefix;
  idx_t prefix_filled;
  bool stream_complete;
};

static PrefixInflateResult InflatePrefix(mz_zip_reader_extract_iter_state *iter,
                                         idx_t prefix_capacity,
                                         idx_t uncomp_size,
                                         const StreamingOptions &options) {
  PrefixInflateResult r;
  r.prefix =
      make_uniq_array2<data_t>(prefix_capacity > 0 ? prefix_capacity : 1);
  r.prefix_filled = 0;
  r.stream_complete = false;

  LineScanner scanner(options.new_line);
  constexpr size_t CHUNK = 64 * 1024;

  while (r.prefix_filled < prefix_capacity && scanner.count < options.lines) {
    size_t want = std::min<size_t>(CHUNK, prefix_capacity - r.prefix_filled);
    size_t got = mz_zip_reader_extract_iter_read(
        iter, r.prefix.get() + r.prefix_filled, want);
    if (got == 0) {
      // EOF before the stopping condition fired. Could be short read on a
      // corrupt archive, or a legitimate entry smaller than our cap.
      r.stream_complete = true;
      break;
    }
    scanner.Scan(r.prefix.get() + r.prefix_filled, got);
    r.prefix_filled += got;
    if (r.prefix_filled >= uncomp_size) {
      r.stream_complete = true;
      break;
    }
  }
  return r;
}
} // namespace

unique_ptr<FileHandle>
StreamingZipFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                 optional_ptr<FileOpener> opener) {
  if (!flags.OpenForReading() || flags.OpenForWriting()) {
    throw IOException("Zip-stream file system can only open for reading");
  }

  ParsedZipPath parsed;
  if (!DetectSchemeAndOptions(path, parsed) || !parsed.streaming) {
    throw IOException("Internal error: StreamingZipFileSystem::OpenFile called "
                      "with non-streaming path '%s'",
                      path);
  }

  auto context = opener->TryGetClientContext();
  const auto paths = SplitArchivePath(parsed.inner_body, *context);
  const auto &zip_path = paths.first;
  const auto &file_path = paths.second;

  auto &fs = FileSystem::GetFileSystem(*context);
  auto handle = fs.OpenFile(zip_path, flags);
  if (!handle) {
    throw IOException("Failed to open file: %s", zip_path);
  }

  if (file_path.empty()) {
    return handle;
  }

  auto normalized_file_path = StringUtil::Replace(
      file_path, fs.PathSeparator(file_path), ZIP_SEPARATOR);

  if (!handle->CanSeek()) {
    throw IOException("Cannot seek");
  }

  idx_t size = handle->GetFileSize();

  // Heap-allocate the archive so its address is stable: miniz's iterator
  // stores a raw pointer back to the archive, and a stack-local copied by
  // value into the handle would leave that pointer dangling when OpenFile
  // returns.
  auto zip = make_uniq<mz_zip_archive>();
  mz_zip_zero_struct(zip.get());
  zip->m_pRead = &FileSystemZipReadFunc;
  zip->m_pIO_opaque = handle.get();

  mz_zip_reader_extract_iter_state *iter = nullptr;
  try {
    mz_uint zip_flags = 0;

    if (!mz_zip_reader_init(zip.get(), size, zip_flags)) {
      throw IOException(
          "Could not open as zip file: %s",
          mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
    }

    mz_uint file_index = 0;
    auto locate_failed =
        mz_zip_reader_locate_file_v2(zip.get(), normalized_file_path.c_str(),
                                     nullptr, 0, &file_index) == MZ_FALSE;
    if (locate_failed) {
      throw IOException("Failed to find file: %s", normalized_file_path);
    }

    mz_zip_archive_file_stat file_stat = {0};
    auto stat_failed =
        mz_zip_reader_file_stat(zip.get(), file_index, &file_stat) == MZ_FALSE;
    if (stat_failed) {
      throw IOException(
          "Problem stat-ing file within archive: %s",
          mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
    }
    if ((file_stat.m_method) && (file_stat.m_method != MZ_DEFLATED)) {
      throw IOException("Unknown compression method");
    }

    const idx_t uncomp_size = UnsafeNumericCast<idx_t>(file_stat.m_uncomp_size);
    const idx_t prefix_capacity =
        MinValue(parsed.options.max_bytes, uncomp_size);

    iter = mz_zip_reader_extract_iter_new(zip.get(), file_index, 0);
    if (iter == nullptr) {
      throw IOException(
          "Failed to initialize streaming reader for '%s' in archive '%s': %s",
          normalized_file_path, zip_path,
          mz_zip_get_error_string(mz_zip_get_last_error(zip.get())));
    }

    auto inflated =
        InflatePrefix(iter, prefix_capacity, uncomp_size, parsed.options);

    if (inflated.prefix_filled >= uncomp_size || inflated.stream_complete) {
      // Degenerate fast-path: the entry fit entirely in the prefix buffer.
      // Tear down the streaming state and hand back a regular ZipFileHandle
      // bound to the *seekable* sibling FS so DuckDB's parallel readers can
      // engage on this handle exactly as they would for `zip://`.
      mz_zip_reader_extract_iter_free(iter);
      iter = nullptr;
      mz_zip_reader_end(zip.get());
      return make_uniq<ZipFileHandle>(seekable_fs, path, flags,
                                      std::move(handle), file_stat,
                                      std::move(inflated.prefix));
    }

    // True streaming path: transfer `zip` (heap-owned) and `iter` into the
    // handle.
    auto result = make_uniq<StreamingZipFileHandle>(
        *this, path, flags, std::move(handle), std::move(zip), iter,
        parsed.options, std::move(inflated.prefix), inflated.prefix_filled,
        uncomp_size, zip_path, normalized_file_path,
        /*stream_complete=*/false);
    // Ownership of `zip` and `iter` now belongs to `result`.
    iter = nullptr;
    return result;
  } catch (Exception &ex) {
    if (iter != nullptr) {
      mz_zip_reader_extract_iter_free(iter);
    }
    if (zip) {
      mz_zip_reader_end(zip.get());
    }
    throw;
  }
}

//------------------------------------------------------------------------------
// Streaming Read/Seek: prefix-vs-stream boundary rules
//------------------------------------------------------------------------------

namespace {
[[noreturn]] void ThrowBackwardSeek(StreamingZipFileHandle &h,
                                    idx_t requested) {
  throw IOException(
      "Backward seek in zip-stream://: requested offset %llu is before the "
      "current streaming position %llu (prefix_filled=%llu) for entry '%s' in "
      "archive '%s'. Use zip:// instead of zip-stream:// to load this file "
      "fully into memory for random access.",
      (unsigned long long)requested, (unsigned long long)h.stream_position,
      (unsigned long long)h.prefix_filled, h.entry_path, h.archive_path);
}

static void SkipForward(StreamingZipFileHandle &h, idx_t bytes) {
  constexpr size_t SCRATCH = 64 * 1024;
  data_t scratch[SCRATCH];
  while (bytes > 0) {
    size_t want = (bytes < SCRATCH) ? bytes : SCRATCH;
    size_t got = mz_zip_reader_extract_iter_read(h.iter, scratch, want);
    if (got == 0) {
      throw IOException(
          "Short read while skipping forward in entry '%s' of archive '%s': %s",
          h.entry_path, h.archive_path,
          mz_zip_get_error_string(mz_zip_get_last_error(h.zip.get())));
    }
    h.stream_position += got;
    bytes -= got;
  }
}

static void ReadFromStream(StreamingZipFileHandle &h, data_t *buffer, idx_t n) {
  idx_t have = 0;
  while (have < n) {
    size_t got =
        mz_zip_reader_extract_iter_read(h.iter, buffer + have, n - have);
    if (got == 0) {
      if (h.stream_position >= h.uncomp_size) {
        // Genuine EOF — tolerate short reads at the tail.
        break;
      }
      throw IOException(
          "Short read from zip entry '%s' in archive '%s': %s", h.entry_path,
          h.archive_path,
          mz_zip_get_error_string(mz_zip_get_last_error(h.zip.get())));
    }
    have += got;
    h.stream_position += got;
  }
}

static idx_t ReadAtLocation(StreamingZipFileHandle &h, void *buffer_in, idx_t n,
                            idx_t location) {
  auto *buffer = static_cast<data_t *>(buffer_in);
  if (location >= h.uncomp_size || n == 0) {
    return 0;
  }
  if (location + n > h.uncomp_size) {
    n = h.uncomp_size - location;
  }

  const idx_t p = location;
  const idx_t end = p + n;

  if (end <= h.prefix_filled) {
    // Fully inside prefix.
    memcpy(buffer, h.prefix.get() + p, n);
    return n;
  }

  if (p >= h.prefix_filled) {
    // Fully past prefix.
    if (p < h.stream_position) {
      ThrowBackwardSeek(h, p);
    }
    if (p > h.stream_position) {
      SkipForward(h, p - h.stream_position);
    }
    idx_t before = h.stream_position;
    ReadFromStream(h, buffer, n);
    return h.stream_position - before;
  }

  // Straddles the prefix/stream boundary.
  const idx_t first_part = h.prefix_filled - p;
  memcpy(buffer, h.prefix.get() + p, first_part);
  if (h.stream_position != h.prefix_filled) {
    ThrowBackwardSeek(h, h.prefix_filled);
  }
  idx_t before = h.stream_position;
  ReadFromStream(h, buffer + first_part, n - first_part);
  return first_part + (h.stream_position - before);
}
} // namespace

void StreamingZipFileSystem::Read(FileHandle &handle, void *buffer,
                                  int64_t nr_bytes, idx_t location) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  ReadAtLocation(h, buffer, UnsafeNumericCast<idx_t>(nr_bytes), location);
}

int64_t StreamingZipFileSystem::Read(FileHandle &handle, void *buffer,
                                     int64_t nr_bytes) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  idx_t got = ReadAtLocation(h, buffer, UnsafeNumericCast<idx_t>(nr_bytes),
                             h.logical_position);
  h.logical_position += got;
  return UnsafeNumericCast<int64_t>(got);
}

int64_t StreamingZipFileSystem::GetFileSize(FileHandle &handle) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  return UnsafeNumericCast<int64_t>(h.uncomp_size);
}

void StreamingZipFileSystem::Seek(FileHandle &handle, idx_t location) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  // Only mutate the logical cursor. The stream is consulted on the next Read
  // that actually needs bytes past the prefix, which lets the sniffer's
  // Read -> Reset -> re-read pattern run entirely out of the prefix buffer.
  h.logical_position = location;
}

void StreamingZipFileSystem::Reset(FileHandle &handle) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  h.logical_position = 0;
}

idx_t StreamingZipFileSystem::SeekPosition(FileHandle &handle) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  return h.logical_position;
}

timestamp_t StreamingZipFileSystem::GetLastModifiedTime(FileHandle &handle) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  auto &inner_handle = *h.inner_handle;
  return inner_handle.file_system.GetLastModifiedTime(inner_handle);
}

FileType StreamingZipFileSystem::GetFileType(FileHandle &handle) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  auto &inner_handle = *h.inner_handle;
  return inner_handle.file_system.GetFileType(inner_handle);
}

bool StreamingZipFileSystem::OnDiskFile(FileHandle &handle) {
  auto &h = handle.Cast<StreamingZipFileHandle>();
  return h.inner_handle->OnDiskFile();
}

vector<OpenFileInfo> StreamingZipFileSystem::Glob(const string &path,
                                                  FileOpener *opener) {
  ParsedZipPath parsed;
  if (!DetectSchemeAndOptions(path, parsed) || !parsed.streaming) {
    throw IOException(
        "Internal error: StreamingZipFileSystem::Glob on non-streaming path "
        "'%s'",
        path);
  }
  return GlobZip(opener, STREAM_SCHEME, parsed.options_literal,
                 parsed.inner_body);
}

bool StreamingZipFileSystem::FileExists(const string &filename,
                                        optional_ptr<FileOpener> opener) {
  ParsedZipPath parsed;
  if (!DetectSchemeAndOptions(filename, parsed) || !parsed.streaming) {
    return false;
  }
  return FileExistsZip(opener, parsed.inner_body);
}

} // namespace duckdb
