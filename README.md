---
type: Repository Guide
title: Lightweight Parquet Reader
description: Reads the string columns of a parquet file in C99, with libzstd as its only dependency.
status: draft
tags: [parquet, host, c]
generated:
  by: claude-code/opus-5
  at: 2026-08-27T10:20:00Z
supervised:
  by: human:ciprian-florin_ifrim
  at: 2026-08-27T10:20:00Z
---

# Lightweight Parquet Reader

A small C99 library that reads the string columns of a parquet file, with libzstd as its only dependency. It exists for C programs that only need the text inside parquet files, such as a tokenizer or a data cleaning step.

Apache Arrow reads every part of the format, but it depends on thirteen other libraries, among them abseil, grpc, llvm, openssl and protobuf. This library needs only libzstd, which is 0.6 MB.

1,554 parquet files were surveyed to decide what the reader must support. From that survey, it reads ZSTD and uncompressed pages, the PLAIN and RLE_DICTIONARY encodings, and BYTE_ARRAY columns, which hold strings. A file that uses anything else is refused, and the error names the codec or encoding it found.

| Layer | Contents |
|---|---|
| `thrift` | a reader for the Thrift compact protocol, which parquet uses for its footer and page headers |
| `meta` | the footer, reduced to what is needed to find a column and decode it |
| `rle` | the run length and bit packed hybrid encoding, which carries null flags and dictionary indices |
| `parquet` | the public API: open a file, then read a string column by name, one row group at a time |

**The reader stops with a clear error on anything it cannot decode, because a misread page would return text that looks real.**

## 1. Build and Run

    make                                   # the library, the tests and the tools
    make test                              # the unit suites
    build/dump-meta FILE.parquet           # the footer, one line per field and chunk
    build/dump-strings FILE.parquet text   # a digest of one column, per row group
    build/peek-strings FILE.parquet text 5 # the first values of a column, to check by eye

Two scripts check the reader against pyarrow, field by field and byte by byte:

    python tools/compare_meta.py build/dump-meta FILE.parquet ...
    python tools/compare_strings.py build/dump-strings text FILE.parquet ...

Both need `pyarrow` installed. They are test tools, separate from the library.

## 2. Directory Tree

    src/        the library, with one header for each source file
    test/       the unit suites, the harness and the committed fixtures
    tools/      the pyarrow comparison scripts and the fixture generator

## 3. Concepts

| Term | Meaning |
|---|---|
| Row group | a horizontal slice of the file. A column is read one row group at a time, because a whole column of a large file may not fit in memory |
| Column chunk | one column inside one row group. The footer gives its offset, its length and its size after decompression |
| Page | the unit in which a chunk is written: an optional dictionary page, then data pages. A writer can change the encoding inside a chunk, for example to PLAIN when the dictionary grows too large |

## 4. Rules

**Copy any value that is needed after the next read.** Every call to `parquet_read_strings` reuses the same memory, so the next call overwrites the values from the last one.

**Allocate a chunk's whole buffer before reading its pages.** Values point into the decompressed pages, so the buffer must not move while a chunk is read. The size comes from `total_uncompressed_size` in the footer, and the test `a_value_of_an_early_page_survives_the_later_pages` checks this.

**Read only the requested column.** The footer gives the position of every column, so the reader skips the others without decoding them.

**Add an encoding only with a test file that uses it.** `tools/write_shapes.py` generates the test files, and each one covers a case that once caused a bug.

## 5. Language Standard

The code is C99 and builds cleanly with `-std=c99 -Wall -Wextra -Wpedantic`. It follows four rules throughout:

    one exit per function, at the bottom
    functions short enough to fit on one screen
    at most two levels of nested blocks
    a comment on every cast that says why it is safe

In a parser these rules matter most, because a function with four return points is where a buffer leaks.

## 6. Limitations

| Limitation | Reason |
|---|---|
| Numeric, nested and repeated columns | The reader is built to extract text, so it refuses any other column type with a clear error |
| SNAPPY, GZIP, BROTLI and LZ4 compression | None of the 1,554 surveyed files used them. Each is refused by name, so support for one has to be added on purpose |
| DELTA_LENGTH_BYTE_ARRAY and the other Parquet 2 encodings | Newer writers can store strings this way, so this is the first encoding to add when such files appear |
| File paths | `parquet_open` takes the file's bytes, so the caller reads the file into memory or maps it with `mmap` first |

## 7. Measurements

Measured on 2026-08-27 over 1,554 parquet files, with the build from section 1.

| Measure | Value |
|---|---|
| Parquet files | 1,554 |
| Compression of every sampled column chunk | ZSTD |
| Encodings | PLAIN, RLE, RLE_DICTIONARY |
| Footers compared with pyarrow, field for field | 122, all equal |
| String columns compared with pyarrow, byte for byte | 29 files, all equal |
| Library source | 2,403 lines |
| Leaks, under `leaks --atExit` | 0, over the suite and a full read of the measured files |
| Undefined behaviour, under `-fsanitize=undefined` | 0 reports |
| Unit checks | 95, over three suites |
| Dependency | libzstd, 0.6 MB |
