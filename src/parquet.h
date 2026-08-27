/*
 * parquet.h: read the string columns of a parquet file.
 *
 * Purpose.
 *
 * Apache Arrow reads parquet correctly and completely. It also depends on
 * abseil, aws-crt-cpp, aws-sdk-cpp, brotli, grpc, llvm, lz4, openssl, protobuf,
 * re2, snappy, thrift and utf8proc, to return a column of strings. This reader
 * only depends on libzstd, the 0.6 MB reference implementation of the ZSTD
 * codec, in C.
 *
 * Scope.
 *
 * The reader decodes ZSTD and uncompressed pages, the PLAIN and RLE_DICTIONARY
 * encodings, and columns of BYTE_ARRAY, the physical type of a string. A
 * measurement of 1,554 parquet files showed that every file uses only these.
 *
 * The reader refuses anything else by name, and it never guesses. A silent
 * misread of a page returns plausible text, and that is the worst failure,
 * because code downstream cannot detect it.
 *
 * Limits.
 *
 * The reader does not write files, and it only reads the columns that a caller
 * names. The footer gives the start and the length of each column chunk, so
 * the reader never touches any other column, even to skip it. It does not
 * decode nested columns, repeated columns or numeric values.
 *
 * Ownership of the bytes.
 *
 * A value points into memory that the reader owns, and the next call to
 * parquet_read_strings replaces it. A call for a different column or a
 * different row group reuses the same memory. A caller that needs a value to
 * outlive that call copies it.
 *
 * The reader itself never copies a value, because the files that it targets
 * are together larger than memory and it exists to walk them.
 *
 * The narrower promise, that only a call for the same row group replaces a
 * value, is the obvious assumption, and it is false. If a caller reads one
 * column, then another, and then uses the first column's values, those values
 * hold real text from the second column.
 */
#ifndef PARQUET_H
#define PARQUET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct parquet_file parquet_file_t;

/* One value of a string column. p_bytes is NULL when the value is null. */
typedef struct
{
    const uint8_t *p_bytes;
    size_t         len;
} parquet_string_t;

/*
 * Open a parquet file that is already in memory.
 *
 * The bytes are borrowed and must outlive the reader. NULL when the file is
 * not parquet or its footer is malformed, with the reason on standard error.
 */
parquet_file_t *parquet_open(const uint8_t *p_bytes, size_t len);

/* Free everything that the reader holds. Safe on NULL. */
void parquet_close(parquet_file_t *p_file);

/* How many rows the file holds, over all its row groups. */
int64_t parquet_rows(const parquet_file_t *p_file);

/* How many row groups. A column is read one row group at a time. */
size_t parquet_row_groups(const parquet_file_t *p_file);

/* How many values this row group holds. Zero when it is past the end. */
size_t parquet_row_group_rows(const parquet_file_t *p_file, size_t row_group);

/* True when the file holds a top level column with this name. */
bool parquet_has_column(const parquet_file_t *p_file, const char *p_name);

/*
 * Read every value of one string column in one row group.
 *
 * Writes at most cap values and stores the number written in p_count. False
 * when the column is absent, is not a string column, uses a feature that the
 * reader does not decode, or has malformed page bytes. The reason goes to
 * standard error and names the codec or the encoding that the reader refused.
 */
bool parquet_read_strings(parquet_file_t *p_file, const char *p_name,
                          size_t row_group, parquet_string_t *p_out,
                          size_t cap, size_t *p_count);

#endif /* PARQUET_H */
