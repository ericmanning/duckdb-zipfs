#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include <miniz/miniz.h>
#include <miniz/miniz_zip.h>

namespace duckdb {

enum class NewLineMode { AUTO, LF, CR, CRLF };

struct StreamingOptions {
  idx_t lines = 20480;
  NewLineMode new_line = NewLineMode::AUTO;
  idx_t max_bytes = 64ULL * 1024 * 1024;
};

class ZipFileHandle final : public FileHandle {
  friend class ZipFileSystem;
  friend class StreamingZipFileSystem;

public:
  ZipFileHandle(FileSystem &file_system, const string &path,
                FileOpenFlags flags, unique_ptr<FileHandle> inner_handle_p,
                const mz_zip_archive_file_stat &file_stat,
                unique_ptr<data_t[]> data)
      : FileHandle(file_system, path, flags),
        inner_handle(std::move(inner_handle_p)), file_stat(file_stat),
        data(std::move(data)), seek_offset(0) {}

  void Close() override;

private:
  unique_ptr<FileHandle> inner_handle;
  mz_zip_archive_file_stat file_stat;
  unique_ptr<data_t[]> data;
  idx_t seek_offset;
};

// Forward-only handle backed by a live miniz extract iterator.
// Owns, in destruction order: iter -> mz_zip_archive -> inner FileHandle.
// `inner_handle` must outlive `zip` because miniz callbacks reach into it
// via m_pIO_opaque. `zip` is heap-allocated so its address is stable — the
// iter's internal pZip pointer references this exact location, and a stack
// copy moved into the handle would leave the iter dangling.
class StreamingZipFileHandle final : public FileHandle {
  friend class StreamingZipFileSystem;

public:
  StreamingZipFileHandle(FileSystem &file_system, const string &path,
                         FileOpenFlags flags,
                         unique_ptr<FileHandle> inner_handle_p,
                         unique_ptr<mz_zip_archive> zip_archive,
                         mz_zip_reader_extract_iter_state *iter,
                         StreamingOptions options, unique_ptr<data_t[]> prefix,
                         idx_t prefix_filled, idx_t uncomp_size,
                         string archive_path, string entry_path,
                         bool stream_complete)
      : FileHandle(file_system, path, flags),
        inner_handle(std::move(inner_handle_p)), zip(std::move(zip_archive)),
        iter(iter), options(options), prefix(std::move(prefix)),
        prefix_filled(prefix_filled), stream_position(prefix_filled),
        logical_position(0), uncomp_size(uncomp_size),
        archive_path(std::move(archive_path)),
        entry_path(std::move(entry_path)), stream_complete(stream_complete) {}

  ~StreamingZipFileHandle() override;
  void Close() override;
  void FreeStream();

  unique_ptr<FileHandle> inner_handle;
  unique_ptr<mz_zip_archive> zip;
  mz_zip_reader_extract_iter_state *iter;
  StreamingOptions options;
  unique_ptr<data_t[]> prefix;
  idx_t prefix_filled;
  idx_t stream_position; // bytes produced by the iter so far (>= prefix_filled)
  idx_t logical_position;
  idx_t uncomp_size;
  string archive_path;
  string entry_path;
  bool stream_complete;
  bool stream_freed = false;
};

class ZipFileSystem final : public FileSystem {
public:
  explicit ZipFileSystem() : FileSystem() {}

  timestamp_t GetLastModifiedTime(FileHandle &handle) override;
  FileType GetFileType(FileHandle &handle) override;
  int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
  void Read(FileHandle &handle, void *buffer, int64_t nr_bytes,
            idx_t location) override;
  int64_t GetFileSize(FileHandle &handle) override;
  void Seek(FileHandle &handle, idx_t location) override;
  void Reset(FileHandle &handle) override;
  idx_t SeekPosition(FileHandle &handle) override;
  std::string GetName() const override { return "ZipFileSystem"; }
  vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;
  bool FileExists(const string &filename,
                  optional_ptr<FileOpener> opener) override;

  bool CanHandleFile(const string &fpath) override;
  bool OnDiskFile(FileHandle &handle) override;
  bool CanSeek() override;

  unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                  optional_ptr<FileOpener> opener) override;
};

// Handles `zip-stream://[options]/archive/entry`. Reports CanSeek=false so
// DuckDB's parallel readers fall back to single-threaded sequential reads,
// which is what a forward-only DEFLATE iterator can actually serve. When an
// entry fits entirely within the prefix buffer, OpenFile returns a regular
// `ZipFileHandle` bound to `seekable_fs` so the degenerate fast-path retains
// full seekability and parallelism.
class StreamingZipFileSystem final : public FileSystem {
public:
  explicit StreamingZipFileSystem(ZipFileSystem &seekable_fs)
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
  std::string GetName() const override { return "StreamingZipFileSystem"; }
  vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;
  bool FileExists(const string &filename,
                  optional_ptr<FileOpener> opener) override;

  bool CanHandleFile(const string &fpath) override;
  bool OnDiskFile(FileHandle &handle) override;
  bool CanSeek() override { return false; }

  unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                  optional_ptr<FileOpener> opener) override;

private:
  ZipFileSystem &seekable_fs;
};

} // namespace duckdb
