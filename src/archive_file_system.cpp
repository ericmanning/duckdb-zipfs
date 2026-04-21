#include "archive_file_system.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/client_context.hpp"

#ifdef ENABLE_LIBARCHIVE

namespace duckdb {

auto const ZIP_SEPARATOR = "/";
static constexpr const char *ARCHIVE_SCHEME = "archive://";
static constexpr size_t ARCHIVE_SCHEME_LEN = 10;
static constexpr const char *ARCHIVE_STREAM_SCHEME = "archive-stream://";
static constexpr size_t ARCHIVE_STREAM_SCHEME_LEN = 17;

struct ParsedArchivePath {
  bool streaming; // false for archive://, true for archive-stream://
  string inner_body;       // archive path + entry, past scheme and brackets
  string options_literal;  // verbatim bracket segment (incl. brackets), for Glob
  StreamingOptions options;
};

static bool DetectArchiveSchemeAndOptions(const string &fpath,
                                          ParsedArchivePath &out) {
  if (fpath.size() > ARCHIVE_SCHEME_LEN &&
      fpath.compare(0, ARCHIVE_SCHEME_LEN, ARCHIVE_SCHEME) == 0) {
    out.streaming = false;
    out.options_literal.clear();
    out.options = StreamingOptions();
    out.inner_body = fpath.substr(ARCHIVE_SCHEME_LEN);
    return true;
  }
  if (fpath.size() > ARCHIVE_STREAM_SCHEME_LEN &&
      fpath.compare(0, ARCHIVE_STREAM_SCHEME_LEN, ARCHIVE_STREAM_SCHEME) == 0) {
    out.streaming = true;
    ParseBracketedOptions(fpath.substr(ARCHIVE_STREAM_SCHEME_LEN), fpath,
                          out.options, out.options_literal, out.inner_body);
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
    // TODO: What to do with other archive extensions?
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
// Zip File Handle
//------------------------------------------------------------------------------

void ArchiveFileHandle::Close() {}

//------------------------------------------------------------------------------
// Zip File System
//------------------------------------------------------------------------------

bool ArchiveFileSystem::CanHandleFile(const string &fpath) {
  // TODO: Check that we can seek into the file
  return fpath.size() > 10 && fpath.substr(0, 10) == "archive://";
}

/* Returns pointer and size of next block of data from archive. */
la_ssize_t FileSystemZipReadFunc(struct archive *archive, void *clientData,
                                 const void **buffer) {
  LibArchiveHandle *handle = (LibArchiveHandle *)clientData;
  auto readBytes =
      handle->inner_handle->Read(handle->data.get(), handle->data_len);
  *buffer = handle->data.get();
  return UnsafeNumericCast<la_ssize_t>(readBytes);
}

/* Seeks to specified location in the file and returns the position.
 * Whence values are SEEK_SET, SEEK_CUR, SEEK_END from stdio.h.
 * Return ARCHIVE_FATAL if the seek fails for any reason.
 */
la_int64_t FileSystemZipSeekFunc(struct archive *archive, void *clientData,
                                 la_int64_t offset, int whence) {
  LibArchiveHandle *handle = (LibArchiveHandle *)clientData;
  if (whence == SEEK_SET) {
    handle->inner_handle->Seek(offset);
  } else if (whence == SEEK_CUR) {
    handle->inner_handle->Seek(handle->inner_handle->SeekPosition() + offset);
  } else if (whence == SEEK_END) {
    handle->inner_handle->Seek(handle->inner_handle->GetFileSize() + offset);
  } else {
    return ARCHIVE_FATAL;
  }
  // libarchive expects the new absolute position, not ARCHIVE_OK. Returning
  // the wrong thing breaks 7z (which relies on seeks landing at specific
  // byte offsets) but was latent for tar-family formats that don't seek.
  return UnsafeNumericCast<la_int64_t>(handle->inner_handle->SeekPosition());
}

int FileSystemZipOpenFunc(struct archive *archive, void *clientData) {
  return ARCHIVE_OK;
}

int FileSystemZipCloseFunc(struct archive *archive, void *clientData) {
  return ARCHIVE_OK;
}

void ReadArchiveEntryFully(struct archive *archive, struct archive_entry *entry,
                           unique_ptr<data_t[]> *out_data,
                           la_int64_t *out_size) {

  if (archive_entry_size_is_set(entry)) {
    *out_size = archive_entry_size(entry);
    *out_data = make_uniq_array2<data_t>(*out_size);

    auto read_bytes = archive_read_data(archive, out_data->get(), *out_size);
    if (read_bytes < *out_size) {
      throw IOException("Failed to read: %s", archive_error_string(archive));
    }
  } else {
    *out_size = 0;
    la_int64_t read = 0;

    std::vector<std::tuple<data_t *, la_int64_t>> blocks;
    auto data_block = new data_t[BLOCK_SIZE]();

    while (read = archive_read_data(archive, data_block, BLOCK_SIZE),
           read > 0) {
      *out_size += read;
      blocks.push_back(make_pair(data_block, read));
      data_block = new data_t[BLOCK_SIZE]();
    }

    delete[] data_block;

    *out_data = make_uniq_array2<data_t>(*out_size);
    la_int64_t offset = 0;
    for (auto &block_and_size : blocks) {
      auto block = std::get<0>(block_and_size);
      auto size = std::get<1>(block_and_size);
      memcpy((*out_data).get() + offset, block, size);
      offset += size;
      delete[] block;
    }
  }
}

unique_ptr<FileHandle>
ArchiveFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                            optional_ptr<FileOpener> opener) {
  if (!flags.OpenForReading() || flags.OpenForWriting()) {
    throw IOException("Archive file system can only open for reading");
  }

  // Get the path to the zip file
  auto context = opener->TryGetClientContext();
  const auto paths = SplitArchivePath(path.substr(10), *context);
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
  timestamp_t last_modified_time;
  bool has_last_modified_time = true;
  try {
    last_modified_time = fs.GetLastModifiedTime(*handle);
  } catch (NotImplementedException &ex) {
    has_last_modified_time = false;
  }
  auto file_type = fs.GetFileType(*handle);
  auto on_disk_file = handle->OnDiskFile();

  struct archive *archive = archive_read_new();
  try {
    if (archive_read_support_filter_all(archive)) {
      throw IOException("Failed to init libarchive (filter all): %s",
                        archive_error_string(archive));
    }

    if (archive_read_support_format_all(archive)) {
      throw IOException("Failed to init libarchive (format all): %s",
                        archive_error_string(archive));
    }
    unique_ptr<LibArchiveHandle> zipHandle =
        make_uniq<LibArchiveHandle>(std::move(handle));
    // TODO: Add skip?
    if (archive_read_set_seek_callback(archive, FileSystemZipSeekFunc)) {
      throw IOException("Failed to init libarchive (seek callback): %s",
                        archive_error_string(archive));
    }
    if (archive_read_open(archive, zipHandle.get(), &FileSystemZipOpenFunc,
                          &FileSystemZipReadFunc, &FileSystemZipCloseFunc)) {
      throw IOException("Failed to init libarchive (read callback): %s",
                        archive_error_string(archive));
    }
    struct archive_entry *entry = archive_entry_new2(archive);
    try {
      bool found = false;
      while (archive_read_next_header2(archive, entry) == ARCHIVE_OK) {
        auto pathName = archive_entry_pathname(entry);
        if (strcmp(pathName, file_path.c_str()) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        throw IOException("Failed to find file: %s", file_path);
      }

      unique_ptr<data_t[]> read_buf;
      la_int64_t read_buf_size;
      ReadArchiveEntryFully(archive, entry, &read_buf, &read_buf_size);

      auto zip_file_handle = make_uniq<ArchiveFileHandle>(
          *this, path, flags, last_modified_time, has_last_modified_time,
          file_type, on_disk_file, read_buf_size, std::move(read_buf));

      archive_entry_free(entry);
      archive_read_free(archive);

      return zip_file_handle;
    } catch (Exception &ex2) {
      archive_entry_free(entry);
      throw;
    }
  } catch (Exception &ex) {
    archive_read_free(archive);
    throw;
  }
}

void ArchiveFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes,
                             idx_t location) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  auto remaining_bytes = t_handle.sz - location;
  auto to_read = MinValue(UnsafeNumericCast<idx_t>(nr_bytes), remaining_bytes);
  memcpy(buffer, t_handle.data.get() + location, to_read);
}

int64_t ArchiveFileSystem::Read(FileHandle &handle, void *buffer,
                                int64_t nr_bytes) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  auto position = t_handle.seek_offset;
  auto remaining_bytes = t_handle.sz - position;
  auto to_read = MinValue(UnsafeNumericCast<idx_t>(nr_bytes), remaining_bytes);
  memcpy(buffer, t_handle.data.get() + position, to_read);
  t_handle.seek_offset += to_read;
  return to_read;
}

int64_t ArchiveFileSystem::GetFileSize(FileHandle &handle) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  return UnsafeNumericCast<int64_t>(t_handle.sz);
}

void ArchiveFileSystem::Seek(FileHandle &handle, idx_t location) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  t_handle.seek_offset = location;
}

void ArchiveFileSystem::Reset(FileHandle &handle) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  t_handle.seek_offset = 0;
}

idx_t ArchiveFileSystem::SeekPosition(FileHandle &handle) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  return t_handle.seek_offset;
}

bool ArchiveFileSystem::CanSeek() { return true; }

timestamp_t ArchiveFileSystem::GetLastModifiedTime(FileHandle &handle) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  if (t_handle.has_last_modified_time) {
    return t_handle.last_modified_time;
  } else {
    throw NotImplementedException("ArchiveFileSystem: GetLastModifiedTime not "
                                  "implemented on underlying filesystem");
  }
}

FileType ArchiveFileSystem::GetFileType(FileHandle &handle) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  return t_handle.file_type;
}

bool ArchiveFileSystem::OnDiskFile(FileHandle &handle) {
  auto &t_handle = handle.Cast<ArchiveFileHandle>();
  return t_handle.on_disk_file;
}

static vector<OpenFileInfo>
GlobArchive(FileOpener *opener, const string &scheme_literal,
            const string &options_literal, const string &inner_body) {
  auto context = opener->TryGetClientContext();
  auto &fs = FileSystem::GetFileSystem(*context);
  const auto parts = SplitArchivePath(inner_body, *context);
  auto &zip_path = parts.first;
  auto has_glob = FileSystem::HasGlob(zip_path);
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

  // Preserve the bracketed options segment byte-for-byte so rewritten paths
  // flow back into the same streaming FS with the same parsed options.
  const string path_prefix =
      scheme_literal +
      (options_literal.empty() ? string() : options_literal + "/");

  vector<OpenFileInfo> result;
  for (const auto &curr_zip : matching_zips) {
    if (!FileSystem::HasGlob(file_path)) {
      // No glob pattern in the file path, just return the file path
      result.push_back(path_prefix + curr_zip.path + extension +
                       ZIP_SEPARATOR + file_path);
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

    struct archive *archive = archive_read_new();
    try {
      if (archive_read_support_filter_all(archive)) {
        throw IOException("Failed to init libarchive (filter all): %s",
                          archive_error_string(archive));
      }
      if (archive_read_support_format_all(archive)) {
        throw IOException("Failed to init libarchive (format all): %s",
                          archive_error_string(archive));
      }
      unique_ptr<LibArchiveHandle> zipHandle =
          make_uniq<LibArchiveHandle>(std::move(archive_handle));
      // TODO: Add skip?
      if (archive_read_set_seek_callback(archive, FileSystemZipSeekFunc)) {
        throw IOException("Failed to init libarchive (seek callback): %s",
                          archive_error_string(archive));
      }
      if (archive_read_open(archive, zipHandle.get(), &FileSystemZipOpenFunc,
                            &FileSystemZipReadFunc, &FileSystemZipCloseFunc)) {
        throw IOException("Failed to init libarchive (read callback): %s",
                          archive_error_string(archive));
      }
      struct archive_entry *entry = archive_entry_new2(archive);
      try {
        string zip_filename;
        const size_t MAX_FILENAME_LEN = 65536; // = 2**16
        zip_filename.reserve(1024);

        while (archive_read_next_header2(archive, entry) == ARCHIVE_OK) {
          if (archive_entry_mode(entry) & AE_IFDIR) {
            continue;
          }

          if (archive_entry_is_encrypted(entry)) {
            continue;
          }

          auto path_name = archive_entry_pathname(entry);
          // TODO: May have backed out an optimization here
          zip_filename = path_name;

          auto entry_parts = StringUtil::Split(zip_filename, '/');

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
                    "Recursive globs are only supported at the end of archive "
                    "file "
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
              // If the entry is deeper than the pattern (and we havent hit a
              // **), then it is not a match
              match = false;
              break;
            }
          }

          if (match) {
            auto entry_path = path_prefix + curr_zip.path + extension +
                              ZIP_SEPARATOR + zip_filename;
            // Cache here???
            result.push_back(entry_path);
          }
        }

        archive_entry_free(entry);
        archive_read_free(archive);
      } catch (Exception &ex2) {
        archive_entry_free(entry);
        throw;
      }
    } catch (Exception &ex) {
      archive_read_free(archive);
      throw;
    }
  }

  return result;
}

vector<OpenFileInfo> ArchiveFileSystem::Glob(const string &path,
                                             FileOpener *opener) {
  return GlobArchive(opener, ARCHIVE_SCHEME, /*options_literal=*/"",
                     path.substr(ARCHIVE_SCHEME_LEN));
}

static bool FileExistsArchive(optional_ptr<FileOpener> opener,
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

  struct archive *archive = archive_read_new();
  try {
    if (archive_read_support_filter_all(archive)) {
      throw IOException("Failed to init libarchive (filter all): %s",
                        archive_error_string(archive));
    }
    if (archive_read_support_format_all(archive)) {
      throw IOException("Failed to init libarchive (format all): %s",
                        archive_error_string(archive));
    }
    unique_ptr<LibArchiveHandle> zipHandle =
        make_uniq<LibArchiveHandle>(std::move(handle));
    // TODO: Add skip?
    if (archive_read_set_seek_callback(archive, FileSystemZipSeekFunc)) {
      throw IOException("Failed to init libarchive (seek callback): %s",
                        archive_error_string(archive));
    }
    if (archive_read_open(archive, zipHandle.get(), &FileSystemZipOpenFunc,
                          &FileSystemZipReadFunc, &FileSystemZipCloseFunc)) {
      throw IOException("Failed to init libarchive (read callback): %s",
                        archive_error_string(archive));
    }
    struct archive_entry *entry = archive_entry_new2(archive);
    try {
      bool found = false;

      while (archive_read_next_header2(archive, entry) == ARCHIVE_OK) {
        auto pathName = archive_entry_pathname(entry);
        if (strcmp(pathName, file_path.c_str()) == 0) {
          found = true;
          break;
        }
      }

      archive_entry_free(entry);
      archive_read_free(archive);

      return found;
    } catch (Exception &ex2) {
      archive_entry_free(entry);
      throw;
    }
  } catch (IOException &ex) {
    archive_read_free(archive);
    return false;
  } catch (Exception &ex) {
    archive_read_free(archive);
    throw;
  }
}

bool ArchiveFileSystem::FileExists(const string &filename,
                                   optional_ptr<FileOpener> opener) {
  return FileExistsArchive(opener, filename.substr(ARCHIVE_SCHEME_LEN));
}

//------------------------------------------------------------------------------
// Streaming Archive File Handle
//------------------------------------------------------------------------------

void StreamingArchiveFileHandle::FreeStream() {
  if (stream_freed) {
    return;
  }
  if (archive != nullptr) {
    archive_read_free(archive);
    archive = nullptr;
  }
  stream_freed = true;
}

StreamingArchiveFileHandle::~StreamingArchiveFileHandle() { FreeStream(); }

void StreamingArchiveFileHandle::Close() {
  FreeStream();
  // inner FileHandle is owned by lib_handle; RAII when lib_handle goes away.
}

//------------------------------------------------------------------------------
// Streaming Archive File System (archive-stream://)
//------------------------------------------------------------------------------

bool StreamingArchiveFileSystem::CanHandleFile(const string &fpath) {
  return fpath.size() > ARCHIVE_STREAM_SCHEME_LEN &&
         fpath.compare(0, ARCHIVE_STREAM_SCHEME_LEN, ARCHIVE_STREAM_SCHEME) ==
             0;
}

namespace {
struct ArchivePrefixInflateResult {
  unique_ptr<data_t[]> prefix;
  idx_t prefix_filled;
  bool stream_complete;
};

static ArchivePrefixInflateResult
InflateArchivePrefix(struct archive *archive, idx_t prefix_capacity,
                     bool size_known, idx_t uncomp_size,
                     const StreamingOptions &options) {
  ArchivePrefixInflateResult r;
  r.prefix =
      make_uniq_array2<data_t>(prefix_capacity > 0 ? prefix_capacity : 1);
  r.prefix_filled = 0;
  r.stream_complete = false;

  LineScanner scanner(options.new_line);
  constexpr size_t CHUNK = 64 * 1024;

  while (r.prefix_filled < prefix_capacity && scanner.count < options.lines) {
    size_t want = std::min<size_t>(CHUNK, prefix_capacity - r.prefix_filled);
    la_ssize_t got =
        archive_read_data(archive, r.prefix.get() + r.prefix_filled, want);
    if (got < 0) {
      throw IOException("libarchive read error: %s",
                        archive_error_string(archive));
    }
    if (got == 0) {
      r.stream_complete = true;
      break;
    }
    scanner.Scan(r.prefix.get() + r.prefix_filled,
                 UnsafeNumericCast<size_t>(got));
    r.prefix_filled += UnsafeNumericCast<idx_t>(got);
    if (size_known && r.prefix_filled >= uncomp_size) {
      r.stream_complete = true;
      break;
    }
  }
  return r;
}
} // namespace

unique_ptr<FileHandle>
StreamingArchiveFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                     optional_ptr<FileOpener> opener) {
  if (!flags.OpenForReading() || flags.OpenForWriting()) {
    throw IOException("Archive-stream file system can only open for reading");
  }

  ParsedArchivePath parsed;
  if (!DetectArchiveSchemeAndOptions(path, parsed) || !parsed.streaming) {
    throw IOException(
        "Internal error: StreamingArchiveFileSystem::OpenFile called with "
        "non-streaming path '%s'",
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

  timestamp_t last_modified_time;
  bool has_last_modified_time = true;
  try {
    last_modified_time = fs.GetLastModifiedTime(*handle);
  } catch (NotImplementedException &ex) {
    has_last_modified_time = false;
  }
  auto file_type = fs.GetFileType(*handle);
  auto on_disk_file = handle->OnDiskFile();

  struct archive *archive = archive_read_new();
  unique_ptr<LibArchiveHandle> lib_handle;
  try {
    if (archive_read_support_filter_all(archive)) {
      throw IOException("Failed to init libarchive (filter all): %s",
                        archive_error_string(archive));
    }
    if (archive_read_support_format_all(archive)) {
      throw IOException("Failed to init libarchive (format all): %s",
                        archive_error_string(archive));
    }
    lib_handle = make_uniq<LibArchiveHandle>(std::move(handle));
    if (archive_read_set_seek_callback(archive, FileSystemZipSeekFunc)) {
      throw IOException("Failed to init libarchive (seek callback): %s",
                        archive_error_string(archive));
    }
    if (archive_read_open(archive, lib_handle.get(), &FileSystemZipOpenFunc,
                          &FileSystemZipReadFunc, &FileSystemZipCloseFunc)) {
      throw IOException("Failed to init libarchive (read callback): %s",
                        archive_error_string(archive));
    }

    struct archive_entry *entry = archive_entry_new2(archive);
    bool size_known = false;
    idx_t uncomp_size = 0;
    try {
      bool found = false;
      while (archive_read_next_header2(archive, entry) == ARCHIVE_OK) {
        if (archive_entry_is_encrypted(entry)) {
          auto pathName = archive_entry_pathname(entry);
          if (strcmp(pathName, file_path.c_str()) == 0) {
            throw IOException(
                "Entry '%s' in archive '%s' is encrypted. libarchive can "
                "detect but not decrypt archive encryption; decrypt the "
                "archive externally before reading.",
                file_path, zip_path);
          }
          continue;
        }
        auto pathName = archive_entry_pathname(entry);
        if (strcmp(pathName, file_path.c_str()) == 0) {
          found = true;
          if (archive_entry_size_is_set(entry)) {
            size_known = true;
            uncomp_size =
                UnsafeNumericCast<idx_t>(archive_entry_size(entry));
          }
          break;
        }
      }
      if (!found) {
        throw IOException("Failed to find file: %s", file_path);
      }
    } catch (Exception &ex) {
      archive_entry_free(entry);
      throw;
    }

    // `entry` remains owned by libarchive for the rest of the stream; we free
    // it explicitly below once we commit to a return path.

    const idx_t prefix_capacity =
        size_known ? MinValue(parsed.options.max_bytes, uncomp_size)
                   : parsed.options.max_bytes;

    auto inflated = InflateArchivePrefix(archive, prefix_capacity, size_known,
                                         uncomp_size, parsed.options);

    // Degenerate fast-path: the iter returned zero before the stopping
    // condition, meaning the entry fit entirely in the prefix. Tear down the
    // streaming state and hand back a regular ArchiveFileHandle bound to the
    // seekable sibling so parallel readers can engage.
    if (inflated.stream_complete) {
      archive_entry_free(entry);
      archive_read_free(archive);
      const idx_t final_size = inflated.prefix_filled;
      return make_uniq<ArchiveFileHandle>(
          seekable_fs, path, flags, last_modified_time, has_last_modified_time,
          file_type, on_disk_file, final_size, std::move(inflated.prefix));
    }

    archive_entry_free(entry);

    // True streaming path. Hand libarchive state + wrapper to the handle.
    auto result = make_uniq<StreamingArchiveFileHandle>(
        *this, path, flags, last_modified_time, has_last_modified_time,
        file_type, on_disk_file, archive, std::move(lib_handle), parsed.options,
        std::move(inflated.prefix), inflated.prefix_filled, uncomp_size,
        size_known, zip_path, file_path);
    archive = nullptr; // ownership transferred
    return result;
  } catch (Exception &ex) {
    if (archive != nullptr) {
      archive_read_free(archive);
    }
    throw;
  }
}

namespace {
[[noreturn]] void ThrowBackwardSeekArchive(StreamingArchiveFileHandle &h,
                                           idx_t requested) {
  throw IOException(
      "Backward seek in archive-stream://: requested offset %llu is before "
      "the current streaming position %llu (prefix_filled=%llu) for entry "
      "'%s' in archive '%s'. Use archive:// instead of archive-stream:// to "
      "load this file fully into memory for random access.",
      (unsigned long long)requested, (unsigned long long)h.stream_position,
      (unsigned long long)h.prefix_filled, h.entry_path, h.archive_path);
}

static void SkipForwardArchive(StreamingArchiveFileHandle &h, idx_t bytes) {
  constexpr size_t SCRATCH = 64 * 1024;
  data_t scratch[SCRATCH];
  while (bytes > 0) {
    size_t want = (bytes < SCRATCH) ? bytes : SCRATCH;
    la_ssize_t got = archive_read_data(h.archive, scratch, want);
    if (got < 0) {
      throw IOException(
          "libarchive read error while skipping forward in entry '%s' of "
          "archive '%s': %s",
          h.entry_path, h.archive_path, archive_error_string(h.archive));
    }
    if (got == 0) {
      throw IOException(
          "Short read while skipping forward in entry '%s' of archive '%s'",
          h.entry_path, h.archive_path);
    }
    h.stream_position += UnsafeNumericCast<idx_t>(got);
    bytes -= UnsafeNumericCast<idx_t>(got);
  }
}

static void ReadFromArchiveStream(StreamingArchiveFileHandle &h,
                                  data_t *buffer, idx_t n) {
  idx_t have = 0;
  while (have < n) {
    la_ssize_t got = archive_read_data(h.archive, buffer + have, n - have);
    if (got < 0) {
      throw IOException(
          "libarchive read error from entry '%s' in archive '%s': %s",
          h.entry_path, h.archive_path, archive_error_string(h.archive));
    }
    if (got == 0) {
      if (h.size_known && h.stream_position >= h.uncomp_size) {
        break;
      }
      // Treat as EOF; downstream code handles short reads at tail.
      break;
    }
    have += UnsafeNumericCast<idx_t>(got);
    h.stream_position += UnsafeNumericCast<idx_t>(got);
  }
}

static idx_t ReadArchiveAtLocation(StreamingArchiveFileHandle &h,
                                   void *buffer_in, idx_t n, idx_t location) {
  auto *buffer = static_cast<data_t *>(buffer_in);
  if (n == 0) {
    return 0;
  }
  if (h.size_known) {
    if (location >= h.uncomp_size) {
      return 0;
    }
    if (location + n > h.uncomp_size) {
      n = h.uncomp_size - location;
    }
  }

  const idx_t p = location;
  const idx_t end = p + n;

  if (end <= h.prefix_filled) {
    memcpy(buffer, h.prefix.get() + p, n);
    return n;
  }

  if (p >= h.prefix_filled) {
    if (p < h.stream_position) {
      ThrowBackwardSeekArchive(h, p);
    }
    if (p > h.stream_position) {
      SkipForwardArchive(h, p - h.stream_position);
    }
    idx_t before = h.stream_position;
    ReadFromArchiveStream(h, buffer, n);
    return h.stream_position - before;
  }

  const idx_t first_part = h.prefix_filled - p;
  memcpy(buffer, h.prefix.get() + p, first_part);
  if (h.stream_position != h.prefix_filled) {
    ThrowBackwardSeekArchive(h, h.prefix_filled);
  }
  idx_t before = h.stream_position;
  ReadFromArchiveStream(h, buffer + first_part, n - first_part);
  return first_part + (h.stream_position - before);
}
} // namespace

void StreamingArchiveFileSystem::Read(FileHandle &handle, void *buffer,
                                      int64_t nr_bytes, idx_t location) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  ReadArchiveAtLocation(h, buffer, UnsafeNumericCast<idx_t>(nr_bytes),
                        location);
}

int64_t StreamingArchiveFileSystem::Read(FileHandle &handle, void *buffer,
                                         int64_t nr_bytes) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  idx_t got = ReadArchiveAtLocation(h, buffer,
                                    UnsafeNumericCast<idx_t>(nr_bytes),
                                    h.logical_position);
  h.logical_position += got;
  return UnsafeNumericCast<int64_t>(got);
}

int64_t StreamingArchiveFileSystem::GetFileSize(FileHandle &handle) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  if (!h.size_known) {
    throw IOException(
        "Entry '%s' in archive '%s' has unknown uncompressed size; this "
        "reader requires a known size. Use archive:// (without -stream) to "
        "load the full entry into memory.",
        h.entry_path, h.archive_path);
  }
  return UnsafeNumericCast<int64_t>(h.uncomp_size);
}

void StreamingArchiveFileSystem::Seek(FileHandle &handle, idx_t location) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  h.logical_position = location;
}

void StreamingArchiveFileSystem::Reset(FileHandle &handle) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  h.logical_position = 0;
}

idx_t StreamingArchiveFileSystem::SeekPosition(FileHandle &handle) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  return h.logical_position;
}

timestamp_t StreamingArchiveFileSystem::GetLastModifiedTime(FileHandle &handle) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  if (h.has_last_modified_time) {
    return h.last_modified_time;
  }
  throw NotImplementedException(
      "StreamingArchiveFileSystem: GetLastModifiedTime not implemented on "
      "underlying filesystem");
}

FileType StreamingArchiveFileSystem::GetFileType(FileHandle &handle) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  return h.file_type;
}

bool StreamingArchiveFileSystem::OnDiskFile(FileHandle &handle) {
  auto &h = handle.Cast<StreamingArchiveFileHandle>();
  return h.on_disk_file;
}

vector<OpenFileInfo>
StreamingArchiveFileSystem::Glob(const string &path, FileOpener *opener) {
  ParsedArchivePath parsed;
  if (!DetectArchiveSchemeAndOptions(path, parsed) || !parsed.streaming) {
    throw IOException(
        "Internal error: StreamingArchiveFileSystem::Glob on non-streaming "
        "path '%s'",
        path);
  }
  return GlobArchive(opener, ARCHIVE_STREAM_SCHEME, parsed.options_literal,
                     parsed.inner_body);
}

bool StreamingArchiveFileSystem::FileExists(const string &filename,
                                            optional_ptr<FileOpener> opener) {
  ParsedArchivePath parsed;
  if (!DetectArchiveSchemeAndOptions(filename, parsed) || !parsed.streaming) {
    return false;
  }
  return FileExistsArchive(opener, parsed.inner_body);
}

} // namespace duckdb

#endif // ENABLE_LIBARCHIVE
