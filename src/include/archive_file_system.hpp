#pragma once

#ifdef ENABLE_LIBARCHIVE

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "streaming_options.hpp"
#include <archive.h>
#include <archive_entry.h>
#include "utils.hpp"

namespace duckdb {

la_ssize_t FileSystemZipReadFunc(struct archive *archive, void *clientData,
                                 const void **buffer);

la_int64_t FileSystemZipSeekFunc(struct archive *archive, void *clientData,
                                 la_int64_t offset, int whence);

int FileSystemZipOpenFunc(struct archive *archive, void *clientData);

int FileSystemZipCloseFunc(struct archive *archive, void *clientData);

void ReadArchiveEntryFully(struct archive *_a, struct archive_entry *entry,
                           unique_ptr<data_t[]> *out_data,
                           la_int64_t *out_size);

const size_t BLOCK_SIZE = 1024 * 10;

class LibArchiveHandle final {
public:
  LibArchiveHandle(unique_ptr<FileHandle> inner_handle_p)
      : inner_handle(std::move(inner_handle_p)) {
    data = make_uniq_array2<data_t>(BLOCK_SIZE);
    data_len = BLOCK_SIZE;
  }

  unique_ptr<FileHandle> inner_handle;
  unique_ptr<data_t[]> data;
  size_t data_len;
};

class ArchiveFileHandle final : public FileHandle {
  friend class ArchiveFileSystem;
  friend class RawArchiveFileSystem;

public:
  ArchiveFileHandle(FileSystem &file_system, const string &path,
                    FileOpenFlags flags, timestamp_t &last_modified_time,
                    bool has_last_modified_time, FileType file_type,
                    bool on_disk_file, size_t sz, unique_ptr<data_t[]> data)
      : FileHandle(file_system, path, flags),
        last_modified_time(last_modified_time),
        has_last_modified_time(has_last_modified_time), file_type(file_type),
        on_disk_file(on_disk_file), sz(sz), data(std::move(data)),
        seek_offset(0) {}

  void Close() override;

private:
  timestamp_t last_modified_time;
  bool has_last_modified_time;
  FileType file_type;
  bool on_disk_file;

  size_t sz;
  unique_ptr<data_t[]> data;
  idx_t seek_offset;
};

class ArchiveFileSystem final : public FileSystem {
public:
  explicit ArchiveFileSystem() : FileSystem() {}

  timestamp_t GetLastModifiedTime(FileHandle &handle) override;
  FileType GetFileType(FileHandle &handle) override;
  int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
  void Read(FileHandle &handle, void *buffer, int64_t nr_bytes,
            idx_t location) override;
  int64_t GetFileSize(FileHandle &handle) override;
  void Seek(FileHandle &handle, idx_t location) override;
  void Reset(FileHandle &handle) override;
  idx_t SeekPosition(FileHandle &handle) override;
  std::string GetName() const override { return "ArchiveFileSystem"; }

  vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;
  bool FileExists(const string &filename,
                  optional_ptr<FileOpener> opener) override;

  bool CanHandleFile(const string &fpath) override;
  bool OnDiskFile(FileHandle &handle) override;
  bool CanSeek() override;

  unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                  optional_ptr<FileOpener> opener) override;

private:
};

class RawArchiveFileSystem final : public FileSystem {
public:
  explicit RawArchiveFileSystem() : FileSystem() {}

  timestamp_t GetLastModifiedTime(FileHandle &handle) override;
  FileType GetFileType(FileHandle &handle) override;
  int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
  void Read(FileHandle &handle, void *buffer, int64_t nr_bytes,
            idx_t location) override;
  int64_t GetFileSize(FileHandle &handle) override;
  void Seek(FileHandle &handle, idx_t location) override;
  void Reset(FileHandle &handle) override;
  idx_t SeekPosition(FileHandle &handle) override;
  std::string GetName() const override { return "RawArchiveFileSystem"; }

  vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;
  bool FileExists(const string &filename,
                  optional_ptr<FileOpener> opener) override;

  bool CanHandleFile(const string &fpath) override;
  bool OnDiskFile(FileHandle &handle) override;
  bool CanSeek() override;

  unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                  optional_ptr<FileOpener> opener) override;

private:
};

// Forward-only handle over a libarchive read context. Owns, in destruction
// order: the `struct archive *` read context, the `LibArchiveHandle` wrapper
// carrying the inner FileHandle + read buffer (libarchive's read callbacks
// reach into it), then the inner handle itself via RAII through the wrapper.
class StreamingArchiveFileHandle final : public FileHandle {
  friend class StreamingArchiveFileSystem;

public:
  StreamingArchiveFileHandle(
      FileSystem &file_system, const string &path, FileOpenFlags flags,
      timestamp_t &last_modified_time, bool has_last_modified_time,
      FileType file_type, bool on_disk_file, struct archive *archive,
      unique_ptr<LibArchiveHandle> lib_handle, StreamingOptions options,
      unique_ptr<data_t[]> prefix, idx_t prefix_filled, idx_t uncomp_size,
      bool size_known, string archive_path, string entry_path)
      : FileHandle(file_system, path, flags),
        last_modified_time(last_modified_time),
        has_last_modified_time(has_last_modified_time), file_type(file_type),
        on_disk_file(on_disk_file), archive(archive),
        lib_handle(std::move(lib_handle)), options(options),
        prefix(std::move(prefix)), prefix_filled(prefix_filled),
        stream_position(prefix_filled), logical_position(0),
        uncomp_size(uncomp_size), size_known(size_known),
        archive_path(std::move(archive_path)),
        entry_path(std::move(entry_path)) {}

  ~StreamingArchiveFileHandle() override;
  void Close() override;
  void FreeStream();

  timestamp_t last_modified_time;
  bool has_last_modified_time;
  FileType file_type;
  bool on_disk_file;
  // libarchive read context. Destroyed via archive_read_free, which releases
  // decompressor state and any libarchive-allocated buffers. lib_handle must
  // outlive this because libarchive's callbacks reach into it.
  struct archive *archive;
  unique_ptr<LibArchiveHandle> lib_handle;
  StreamingOptions options;
  unique_ptr<data_t[]> prefix;
  idx_t prefix_filled;
  idx_t stream_position; // bytes produced by archive_read_data so far
  idx_t logical_position;
  // For formats that report size up front, cached from archive_entry_size().
  // When size_known is false (e.g., some 7z entries before decompression
  // completes), uncomp_size is the prefix cap and file size is reported as
  // unknown to readers that ask.
  idx_t uncomp_size;
  bool size_known;
  string archive_path;
  string entry_path;
  bool stream_freed = false;
};

// Handles `archive-stream://[options]/archive/entry`. Reports CanSeek=false
// so DuckDB's parallel readers fall back to single-threaded sequential reads
// (addendum requirement). When an entry fits entirely within the prefix
// buffer, OpenFile tears down the streaming state and returns a regular
// ArchiveFileHandle bound to the seekable sibling FS.
class StreamingArchiveFileSystem final : public FileSystem {
public:
  explicit StreamingArchiveFileSystem(ArchiveFileSystem &seekable_fs)
      : FileSystem(), seekable_fs(seekable_fs) {}

  timestamp_t GetLastModifiedTime(FileHandle &handle) override;
  FileType GetFileType(FileHandle &handle) override;
  int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
  void Read(FileHandle &handle, void *buffer, int64_t nr_bytes,
            idx_t location) override;
  int64_t GetFileSize(FileHandle &handle) override;
  void Seek(FileHandle &handle, idx_t location) override;
  void Reset(FileHandle &handle) override;
  idx_t SeekPosition(FileHandle &handle) override;
  std::string GetName() const override { return "StreamingArchiveFileSystem"; }

  vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;
  bool FileExists(const string &filename,
                  optional_ptr<FileOpener> opener) override;

  bool CanHandleFile(const string &fpath) override;
  bool OnDiskFile(FileHandle &handle) override;
  bool CanSeek() override { return false; }

  unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                  optional_ptr<FileOpener> opener) override;

private:
  ArchiveFileSystem &seekable_fs;
};

} // namespace duckdb

#endif // ENABLE_LIBARCHIVE
