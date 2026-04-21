# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`zipfs` is a DuckDB C++ extension that registers virtual file systems for reading files inside archives (`zip://`, `zip-stream://`, `archive://`, `archive-stream://`, `compressed://`). Built against a pinned DuckDB version via git submodule and the shared `duckdb/extension-ci-tools` build infrastructure.

## Build & test

Bootstrap vcpkg once:
```sh
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Common targets (all forwarded to `extension-ci-tools/makefiles/duckdb_extension.Makefile`):
- `GEN=ninja make release` / `make debug` / `make reldebug` — build the extension
- `make test` / `make test_release` / `make test_debug` — run sqllogic tests
- `make format` — format check (tidy is disabled because it does not work with vcpkg)
- `make update` / `make pull` — sync submodules
- `make wasm_mvp` / `wasm_eh` / `wasm_threads` — WASM builds

Run a single sqllogic test by passing the file to the test runner directly, e.g.:
```sh
build/release/test/unittest test/sql/zipfs_read_csv.test
```

After building, extension binaries are emitted under `build/<config>/extension/zipfs/`. Loading the built `.duckdb_extension` in a fresh DuckDB CLI requires `-unsigned`.

## Architecture

The extension is a single entry point (`src/zipfs_extension.cpp`) that registers sub-filesystems on the DuckDB `VirtualFileSystem` and adds two extension options:
- `zipfs_extension` (default `.zip`): suffix used to split archive path from inner path.
- `zipfs_split` (default NULL): explicit separator string; when set, overrides `zipfs_extension` and enables libarchive-style `archive://foo.tar.gz!!inner.csv` paths.

Five URL schemes map to five `FileSystem` subclasses:

| Scheme | Class | Backed by | Handles |
| --- | --- | --- | --- |
| `zip://` | `ZipFileSystem` (`src/zip_file_system.cpp`) | miniz | zip archives; only one always-available. Eagerly inflates selected entry into memory. |
| `zip-stream://` | `StreamingZipFileSystem` (`src/zip_file_system.cpp`) | miniz | same parser as `zip://`; forward-only read with bounded prefix buffer. |
| `archive://` | `ArchiveFileSystem` (`src/archive_file_system.cpp`) | libarchive | tar, tar.gz, 7z, and other libarchive formats. Eagerly inflates. |
| `archive-stream://` | `StreamingArchiveFileSystem` (`src/archive_file_system.cpp`) | libarchive | streaming counterpart of `archive://`. |
| `compressed://` | `RawArchiveFileSystem` (`src/raw_archive_file_system.cpp`) | libarchive | single-file streams like `.jsonl.bz2`, `.jsonl.gz`. |

### Libarchive is conditional

`ENABLE_LIBARCHIVE` is ON everywhere except Windows (see [CMakeLists.txt](CMakeLists.txt) and the `!windows` constraint in [vcpkg.json](vcpkg.json)). When disabled, the `Noop*` classes from `src/noop_archive_file_system.cpp` are registered instead — they only exist to produce clear errors when someone tries to use `archive://` / `compressed://` on Windows. When touching archive/compressed code, keep changes inside `#ifdef ENABLE_LIBARCHIVE` and mirror any public surface in the noop header.

### Handle shapes

The non-streaming filesystems (`zip://`, `archive://`, `compressed://`) eagerly read the selected inner file fully into a `unique_ptr<data_t[]>` on open and serve `Read`/`Seek` from memory. No central-directory cache, no streaming — globbing reopens and re-parses the archive for each resolved file, and uncompressed file size must fit in memory.

The streaming filesystems (`zip-stream://`, `archive-stream://`) eagerly inflate only a bounded prefix buffer (default 20480 line-delimiter occurrences or 64 MB, whichever first) and then serve reads past the prefix from a forward-only decompressor iterator. They report `CanSeek()=false` so DuckDB's parallel CSV reader falls back to sequential reads — this is a property of DEFLATE, not a limitation of the extension. Each holds a reference to its non-streaming sibling so that on the degenerate fast-path (entry fits fully in the prefix) it can return a regular seekable handle with full within-entry parallelism.

### Shared streaming helpers

`StreamingOptions`, `LineScanner`, the `key=value,...` options parser, and the bracketed-options URL parser all live in [src/include/streaming_options.hpp](src/include/streaming_options.hpp) + [src/streaming_options.cpp](src/streaming_options.cpp) so both streaming filesystems use one implementation. If you add a new stream option, add it there, not in either filesystem's cpp.

### Path splitting

`SplitArchivePath` in [src/zip_file_system.cpp](src/zip_file_system.cpp) is the reference implementation of the `zipfs_split` / `zipfs_extension` logic. The archive filesystems reimplement analogous splitting for their schemes — if you change the splitting semantics in one place, check the others.

### libarchive seek callback

`FileSystemZipSeekFunc` in [src/archive_file_system.cpp](src/archive_file_system.cpp) must return the new absolute byte position from the callback, not `ARCHIVE_OK`. This was a latent bug until 7z support was exercised by the streaming path — tar-family formats never seek, so the wrong return value was invisible. Don't revert.

## Extension name

The extension name is `zipfs` (lowercase), registered via `DUCKDB_CPP_EXTENSION_ENTRY(zipfs, ...)` and `TARGET_NAME` in [CMakeLists.txt](CMakeLists.txt). The DuckDB community extensions catalog entry is also `zipfs`. Do not rename casually — it affects install paths, catalog listings, and the `EXT_VERSION_ZIPFS` macro.

## DuckDB version coupling

Both `duckdb` and `extension-ci-tools` are git submodules pinned to a specific DuckDB release (currently v1.5.0). The CI pipeline ([.github/workflows/MainDistributionPipeline.yml](.github/workflows/MainDistributionPipeline.yml)) builds against both the pinned stable release AND `main`, so changes must compile against both. When bumping DuckDB, update both submodules, the README badge, and the stable version in the workflow together.

## Tests

All tests are DuckDB sqllogic tests under [test/sql/](test/sql/). Fixtures are prebuilt archives in [examples/](examples/) with layouts documented in [examples/README.md](examples/README.md). Windows-specific tests live in `*_windows.test` files and cover the noop filesystems' error behavior.

URLs that pass bracketed stream options through `read_csv` need `hive_partitioning=false` because DuckDB's default hive detection treats any `key=value` in a path segment as a partition column — the bracket group matches. Tests exercising `[...]` options therefore use `read_csv(..., hive_partitioning=false)` explicitly.
