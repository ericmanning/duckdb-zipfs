#include "zipfs_extension.hpp"
#include "zip_file_system.hpp"
#include "archive_file_system.hpp"
#include "noop_archive_file_system.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
  std::string description = "Support for reading files from zip archives";
  loader.SetDescription(description);

  auto &fs = loader.GetDatabaseInstance().GetFileSystem();
  // The streaming FS hands out regular ZipFileHandle instances on its
  // degenerate fast-path and binds them to the seekable sibling so they keep
  // full seekability. Capture the pointer before moving ownership into the
  // VFS; unique_ptr move doesn't relocate the pointee, and the subsystem
  // outlives the streaming FS that references it.
  auto zip_fs_owned = make_uniq<ZipFileSystem>();
  auto &zip_fs_ref = *zip_fs_owned;
  fs.RegisterSubSystem(std::move(zip_fs_owned));
  fs.RegisterSubSystem(make_uniq<StreamingZipFileSystem>(zip_fs_ref));
#ifdef ENABLE_LIBARCHIVE
  // Same seekable-sibling wiring as the zip streaming FS: capture the raw
  // reference before moving ownership into the VFS so the streaming FS can
  // return regular ArchiveFileHandle instances on its degenerate fast-path.
  auto archive_fs_owned = make_uniq<ArchiveFileSystem>();
  auto &archive_fs_ref = *archive_fs_owned;
  fs.RegisterSubSystem(std::move(archive_fs_owned));
  fs.RegisterSubSystem(
      make_uniq<StreamingArchiveFileSystem>(archive_fs_ref));
  fs.RegisterSubSystem(make_uniq<RawArchiveFileSystem>());
#else
  fs.RegisterSubSystem(make_uniq<NoopArchiveFileSystem>());
  fs.RegisterSubSystem(make_uniq<NoopRawArchiveFileSystem>());
#endif // ENABLE_LIBARCHIVE

  auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
  config.AddExtensionOption(
      "zipfs_extension",
      "Extension to look for splitting the zip path and "
      "the file path within the zip. To specify an artificial seperator, "
      "instead set: `set zipfs_split = '!!';`",
      LogicalType::VARCHAR, Value(".zip"));
  config.AddExtensionOption(
      "zipfs_split",
      "Extension to look for splitting the zip path and "
      "the file path within the zip. Will be removed from the zip file name. "
      "Overrides zipfs_extension. Defaults to NULL.",
      LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
}

void ZipfsExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }

std::string ZipfsExtension::Name() { return "zipfs"; }

std::string ZipfsExtension::Version() const {
#ifdef EXT_VERSION_ZIPFS
  return EXT_VERSION_ZIPFS;
#else
  return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(zipfs, loader) { duckdb::LoadInternal(loader); }
}
