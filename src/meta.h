/*
 * meta.h: the parquet file footer, reduced to what a string column needs.
 *
 * A parquet footer describes every column of every row group, with statistics,
 * page indexes, key value metadata and column orders. This module only keeps
 * the fields that give the location of a column's bytes and the way to decode
 * them, and it skips all other fields.
 *
 * File layout.
 *
 * A parquet file opens with the four bytes PAR1 and closes with the footer,
 * then a four byte little endian length, then PAR1 again. The length is read
 * from the end, so the footer is found before any data is read.
 *
 * Kept fields.
 *
 * The schema is kept as a flat list of the top level fields, because a caller
 * addresses a column by name. Each column chunk of each row group also has one
 * record, which holds the codec, the encodings, the value count and both page
 * offsets.
 *
 * Skipped fields.
 *
 * Statistics, page indexes, key value metadata, column orders, and every
 * nested or repeated field are skipped. The caller names the column to read,
 * and the reader only decodes the named column.
 */
#ifndef META_H
#define META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The physical types that parquet writes. Only BYTE_ARRAY is decoded. */
#define META_TYPE_BOOLEAN     0
#define META_TYPE_INT32       1
#define META_TYPE_INT64       2
#define META_TYPE_INT96       3
#define META_TYPE_FLOAT       4
#define META_TYPE_DOUBLE      5
#define META_TYPE_BYTE_ARRAY  6
#define META_TYPE_FIXED_ARRAY 7

/* The codecs parquet defines. Only ZSTD and UNCOMPRESSED are decoded. */
#define META_CODEC_UNCOMPRESSED 0
#define META_CODEC_SNAPPY       1
#define META_CODEC_GZIP         2
#define META_CODEC_LZO          3
#define META_CODEC_BROTLI       4
#define META_CODEC_LZ4          5
#define META_CODEC_ZSTD         6
#define META_CODEC_LZ4_RAW      7

/* The encodings parquet defines. Only PLAIN and RLE_DICTIONARY are decoded. */
#define META_ENC_PLAIN            0
#define META_ENC_PLAIN_DICTIONARY 2
#define META_ENC_RLE              3
#define META_ENC_BIT_PACKED       4
#define META_ENC_DELTA_BINARY     5
#define META_ENC_DELTA_LEN_ARRAY  6
#define META_ENC_DELTA_ARRAY      7
#define META_ENC_RLE_DICTIONARY   8
#define META_ENC_BYTE_STREAM      9

/* The longest column name that the reader keeps. A longer name is refused. */
#define META_NAME_MAX 96U

/* The name of a codec or an encoding, so that a refusal can name it. */
const char *meta_codec_name(int32_t codec);
const char *meta_encoding_name(int32_t encoding);

/*
 * One leaf of the schema, in the order of the column chunks.
 *
 * A parquet schema is a tree, and only its leaves have column chunks. A group
 * in the middle of the tree only holds other fields. The leaves stay in schema
 * order, so a field index is the same as a chunk index.
 */
typedef struct
{
    char    p_name[META_NAME_MAX];
    int32_t type;
    /* 0 required, 1 optional, 2 repeated. Only 0 and 1 can be read. */
    int32_t repetition;
    /*
     * True when the leaf sits below a group other than the root, which is how
     * a list or a struct is written. Such a column needs repetition levels,
     * and the reader does not decode them. The reader refuses the column by
     * name, and never reads it as a flat column.
     */
    bool    b_nested;
} meta_field_t;

/* One column of one row group: where its bytes are and how they are coded. */
typedef struct
{
    int64_t  num_values;
    int64_t  data_page_offset;
    /* Zero when the chunk does not have a dictionary page. */
    int64_t  dictionary_page_offset;
    int64_t  total_compressed_size;
    /*
     * The total size of the chunk's pages after decompression. The reader
     * allocates all of it before it walks the pages. A value of a PLAIN page
     * is borrowed from that memory, so a later page must not move it. If the
     * buffer grew during the walk, every earlier value would keep a correct
     * length and point into freed memory.
     */
    int64_t  total_uncompressed_size;
    int32_t  codec;
    int32_t  type;
    /* One bit for each encoding the chunk declares, so a refusal names it. */
    uint32_t encodings;
} meta_chunk_t;

/* The footer, reduced. */
typedef struct
{
    int32_t       version;
    int64_t       num_rows;
    meta_field_t *p_field;
    size_t        fields;
    /* row_groups by fields, in row group order. */
    meta_chunk_t *p_chunk;
    size_t        row_groups;
} meta_file_t;

/*
 * Read the footer from a whole file in memory.
 *
 * The file is borrowed and never freed. False when it is not a parquet file,
 * or when the footer is malformed. The reason goes to standard error and names
 * what the reader found, because a refusal only helps when it names the byte
 * at fault.
 */
bool meta_read(const uint8_t *p_file, size_t len, meta_file_t *p_out);

/* Free everything that meta_read allocated. */
void meta_free(meta_file_t *p_meta);

/*
 * The index of a top level field with this name, or the field count when the
 * name is absent.
 */
size_t meta_field_of(const meta_file_t *p_meta, const char *p_name);

/* The chunk for this field in this row group. NULL when out of range. */
const meta_chunk_t *meta_chunk_of(const meta_file_t *p_meta, size_t row_group,
                                  size_t field);

#endif /* META_H */
