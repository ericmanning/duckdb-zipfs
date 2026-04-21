[![Extension Test](https://github.com/isaacbrodsky/duckdb-zipfs/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/isaacbrodsky/duckdb-zipfs/actions/workflows/MainDistributionPipeline.yml)
[![DuckDB Version](https://img.shields.io/static/v1?label=duckdb&message=v1.5.0&color=blue)](https://github.com/duckdb/duckdb/releases/tag/v1.5.0)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

This is a [DuckDB](https://duckdb.org) extension that adds support for reading files from within [zip archives](https://en.wikipedia.org/wiki/ZIP_(file_format)) and other archive formats such as `tar`.

# Get started

Load from the [community extensions repository](https://community-extensions.duckdb.org/extensions/zipfs.html):
```SQL
INSTALL zipfs FROM community;
LOAD zipfs;
```

To read a file:
```SQL
SELECT * FROM 'zip://examples/a.zip/a.csv';
```

To read a file from azure blob storage (or other file system):
```SQL
SELECT * FROM 'zip://az://yourstorageaccount.blob.core.windows.net/yourcontainer/examples/a.zip/a.csv';
```

## File names

| URL quick reference | Description
| --- | ---
| `zip://a.zip/*.csv` | Local zip file named `a.zip`, containing csv files.
| `zip://http://example.com/a.zip/*.csv` | Web hosted zip file named `a.zip`, containing csv files.
| `zip-stream://a.zip/big.csv` | Local zip file read with bounded memory via streaming decompression.
| `archive://a.tar.gz!!*.csv` | Local archive file named `a.tar.gz`, containg csv files.
| `compressed://a.jsonl.bz2` | Local compressed ndjson file `a.jsonl.bz2`.

File names passed into the `zip://` URL scheme are expected to end with `.zip`, which indicates the end of the zip file name. The path after
that is taken to be the file path within the zip archive.

Globbing within the zip archive is supported, but see below for performance limitations. A glob query looks like:
```SQL
SELECT * FROM 'zip://examples/a.zip/*.csv';
```

Globbing for multiple zip files:
```SQL
SELECT * FROM 'zip://examples/*.zip/*.csv';
```

You may use options to turn this behavior off and instead choose some string to split on:
```SQL
SET zipfs_split = "!!";

SELECT * FROM 'zip://examples/a.zip!!b.csv';
```

Using `zipfs_split` also means you can read other archives supported by libarchive: (note different URL scheme, and libarchive is not available on Windows)
```SQL
SET zipfs_split = "!!";

SELECT * FROM 'archive://examples/a.tar.gz!!b.csv';
```

It is also possible to read from a variety of compressed file formats directly:
```SQL
SELECT * FROM read_json('compressed://examples/a.jsonl.bz2');
```

## Streaming reads with `zip-stream://`

The `zip-stream://` URL scheme reads an entry in bounded memory instead of inflating the entire entry up front. Use it when an uncompressed entry is larger than you want to hold in memory at once — for example, a multi-gigabyte `.csv` inside a zip on a memory-constrained machine.

```SQL
SELECT * FROM 'zip-stream://examples/a.zip/big.csv';
```

Everything else about zip path parsing — `.zip` splitting, `zipfs_split`, globbing — behaves the same as `zip://`.

### Options

The `zip-stream://` scheme accepts an optional bracketed options segment between the scheme and the archive path:

```
zip-stream://[lines=20480,new_line=\r\n,max_bytes=64MB]/archive.zip/entry.csv
```

| Option | Default | Description
| --- | --- | ---
| `lines` | `20480` | Number of line-delimiter byte occurrences to inflate eagerly into a prefix buffer so the CSV sniffer's sample window is fully in memory. Matches DuckDB's default `sample_size`. Counts raw delimiter bytes, not logical records — files with quoted embedded newlines may need a larger value.
| `new_line` | auto | One of `\n`, `\r`, or `\r\n`. When unset, `\n` is counted (which correctly handles CRLF since every CRLF contains an LF).
| `max_bytes` | `64MB` | Hard cap on the prefix buffer size. Protects against entries with no delimiters. Accepts `KB`/`MB`/`GB` suffixes.

Prefix inflation stops when any of these is met: `lines` delimiters seen, `max_bytes` reached, or end of entry.

Only `\r` and `\n` are recognized as escape sequences inside option values; any other backslash sequence is rejected.

### Reading with options: pass `hive_partitioning=false`

DuckDB's readers treat `key=value` segments in a path as Hive partition columns by default. With the bracketed options syntax, that causes the option pair to be mistakenly surfaced as a phantom column on every row. Disable it explicitly when you pass options:

```SQL
SELECT * FROM read_csv(
  'zip-stream://[max_bytes=64MB]/archive.zip/big.csv',
  hive_partitioning=false
);
```

URLs without brackets (defaults) are unaffected and can be read via the `SELECT * FROM 'path'` sugar with no extra flags.

### Single-threaded within an entry

Streaming handles read one entry forward-only and single-threaded *by design*. DuckDB's parallel CSV reader detects this (the handle reports `CanSeek()` as false) and falls back to sequential reads. This is a consequence of how DEFLATE encodes data — random access into a compressed DEFLATE stream requires either decompressing from offset zero or a chunk-boundary index, and zip files don't carry one. It is not something this extension can work around on an arbitrary zip.

What you still get:

- **Between-entry parallelism:** globbing `zip-stream://archive.zip/*.csv` opens each matched entry on its own handle and DuckDB can parallelize across entries.
- **Degenerate fast-path:** when an entry fits entirely within the prefix buffer, `zip-stream://` returns a fully seekable in-memory handle behind the scenes. Small entries keep full within-entry parallelism.
- **Auto-detection:** `read_csv` auto-detect works because the sniffer's sample window fits inside the prefix buffer; Reset + re-read is served out of memory.

If within-entry parallelism on large entries is essential, either use `zip://` (if the uncompressed entry fits in memory) or preprocess the archive into [SOZip](https://sozip.org) format, which this extension does not yet support but may in the future.

Parquet-in-zip is not supported through `zip-stream://`: the Parquet reader needs random access. Use `zip://` for Parquet entries that fit in memory.

## Performance considerations

This extension is intended more for convience than high performance. It does not implement a file metadata cache as `tarfs` (on which this
extension is based) does. As such, operations which require the central directory (index) of the zip file, such as globbing files, must
reread the central directory multiple times, once for the glob and once for each file to open.

By default, the selected file is read entirely into memory. Use the `zip-stream://` scheme described above to read an entry with bounded memory instead.

# Development

First, install vcpkg to `vcpkg`:

```sh
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Then:

```sh
GEN=ninja make release
make test_release
```

# License

duckdb-zipfs Copyright 2025 Isaac Brodsky. Licensed under the [MIT License](./LICENSE).

[DuckDB](https://github.com/duckdb/duckdb) Copyright 2018-2022 Stichting DuckDB Foundation (MIT License)

[miniz](https://github.com/richgel999/miniz)
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC
(MIT License)

[DuckDB extension-template](https://github.com/duckdb/extension-template) Copyright 2018-2022 DuckDB Labs BV (MIT License)

[duckdb_tarfs](https://github.com/Maxxen/duckdb_tarfs) (MIT license)

[libarchive](https://github.com/libarchive/libarchive)
Copyright 2003-2018 Tim Kientzle
(varying licenses, see repo)
