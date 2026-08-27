/* parquet.h explains the scope, the refusals and the ownership of the bytes. */
#include "parquet.h"
#include "meta.h"
#include "rle.h"
#include "thrift.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/* The page types parquet defines. */
#define PAGE_DATA       0
#define PAGE_INDEX      1
#define PAGE_DICTIONARY 2
#define PAGE_DATA_V2    3

/* A definition level of this value means that the row holds a value. */
#define DEFINED 1

/* A growable buffer that the reader owns and reuses between pages. */
typedef struct
{
    uint8_t *p_bytes;
    size_t   cap;
    size_t   len;
} room_t;

struct parquet_file
{
    const uint8_t *p_file;
    size_t         len;
    meta_file_t    meta;
    /* The current page, and the dictionary of its chunk. */
    room_t         page;
    room_t         dictionary_page;
    parquet_string_t *p_dictionary;
    size_t         dictionary_values;
    size_t         dictionary_cap;
};

/* The fields of one page header that a string column needs. */
typedef struct
{
    int32_t type;
    int32_t compressed_size;
    int32_t uncompressed_size;
    int32_t num_values;
    int32_t encoding;
    /* A version two data page keeps its levels outside the compressed part. */
    int32_t definition_levels_bytes;
    int32_t repetition_levels_bytes;
    bool    b_compressed;
} page_head_t;

/* Grows a buffer, when necessary, to hold at least this many bytes. */
static bool room_for(room_t *p_room, size_t want)
{
    bool b_ok = true;

    if (p_room->cap < want)
    {
        uint8_t *p_new = realloc(p_room->p_bytes, want);

        b_ok = (NULL != p_new);

        if (b_ok)
        {
            p_room->p_bytes = p_new;
            p_room->cap = want;
        }
    }

    return b_ok;
}

static void room_free(room_t *p_room)
{
    free(p_room->p_bytes);
    p_room->p_bytes = NULL;
    p_room->cap = 0U;
    p_room->len = 0U;

    return;
}

/*
 * One PageHeader.
 *
 * Field 5 is a version one data page, field 7 is a dictionary page and field 8
 * is a version two data page. Exactly one of the three is present, and it
 * agrees with the page type in field 1.
 */
static bool read_page_sub(thrift_reader_t *p_reader, page_head_t *p_out,
                          int16_t which)
{
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;

    thrift_struct_begin(p_reader);

    while (thrift_field(p_reader, &type, &id))
    {
        bool b_v2 = (8 == which);

        if (1 == id)
        {
            /* CAST: a value count is a positive 32 bit number here. */
            p_out->num_values = (int32_t)thrift_zigzag(p_reader);
        }
        else if ((2 == id) && !b_v2)
        {
            /* CAST: an encoding is one of ten small values. */
            p_out->encoding = (int32_t)thrift_zigzag(p_reader);
        }
        else if ((4 == id) && b_v2)
        {
            p_out->encoding = (int32_t)thrift_zigzag(p_reader);
        }
        else if ((5 == id) && b_v2)
        {
            p_out->definition_levels_bytes = (int32_t)thrift_zigzag(p_reader);
        }
        else if ((6 == id) && b_v2)
        {
            p_out->repetition_levels_bytes = (int32_t)thrift_zigzag(p_reader);
        }
        else if ((7 == id) && b_v2)
        {
            /* The value is the type, and it defaults to true when absent. */
            p_out->b_compressed = (THRIFT_TRUE == type);
        }
        else
        {
            thrift_skip(p_reader, type);
        }
    }

    thrift_struct_end(p_reader);

    return thrift_ok(p_reader);
}

/* Reads the page header at this offset, and reports its length in bytes. */
static bool read_page_head(const uint8_t *p_at, size_t left,
                           page_head_t *p_out, size_t *p_head_len)
{
    thrift_reader_t reader;
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;
    bool b_ok = true;

    (void)memset(p_out, 0, sizeof(*p_out));
    p_out->b_compressed = true;
    thrift_open(&reader, p_at, left);
    thrift_struct_begin(&reader);

    while (b_ok && thrift_field(&reader, &type, &id))
    {
        if (1 == id)
        {
            /* CAST: a page type is one of four small values. */
            p_out->type = (int32_t)thrift_zigzag(&reader);
        }
        else if (2 == id)
        {
            p_out->uncompressed_size = (int32_t)thrift_zigzag(&reader);
        }
        else if (3 == id)
        {
            p_out->compressed_size = (int32_t)thrift_zigzag(&reader);
        }
        else if ((5 == id) || (7 == id) || (8 == id))
        {
            b_ok = read_page_sub(&reader, p_out, id);
        }
        else
        {
            thrift_skip(&reader, type);
        }
    }

    thrift_struct_end(&reader);
    *p_head_len = reader.at;

    return b_ok && thrift_ok(&reader) &&
           (0 <= p_out->compressed_size) && (0 <= p_out->uncompressed_size);
}

/*
 * Copies the bytes of a page into a readable buffer, and decompresses them when
 * the chunk says so.
 *
 * A codec that the reader does not decode is refused by name. A guess would
 * return bytes that only look like text.
 */
static bool take_page(room_t *p_room, const uint8_t *p_in, size_t in_len,
                      size_t out_len, int32_t codec)
{
    /*
     * The page is appended to an arena that was sized before the walk began.
     * The arena therefore never reallocates, and a value from an earlier page
     * never moves. The size is total_uncompressed_size, from meta.h.
     */
    size_t from = p_room->len;
    bool b_ok = ((from + out_len) <= p_room->cap);

    if (!b_ok)
    {
        (void)fprintf(stderr, "the pages of this chunk exceed the %zu bytes "
                              "that its footer claims\n", p_room->cap);
    }

    if (b_ok && (META_CODEC_UNCOMPRESSED == codec))
    {
        b_ok = (in_len == out_len);

        if (b_ok)
        {
            (void)memcpy(&p_room->p_bytes[from], p_in, in_len);
        }
    }
    else if (b_ok && (META_CODEC_ZSTD == codec))
    {
        size_t got = ZSTD_decompress(&p_room->p_bytes[from], out_len, p_in,
                                     in_len);

        b_ok = (0 == ZSTD_isError(got)) && (got == out_len);

        if (!b_ok)
        {
            (void)fprintf(stderr, "a zstd page did not decompress to the "
                                  "%zu bytes that its header claims\n",
                          out_len);
        }
    }
    else if (b_ok)
    {
        (void)fprintf(stderr, "the reader decodes UNCOMPRESSED and ZSTD "
                              "pages, and this file uses %s\n",
                      meta_codec_name(codec));
        b_ok = false;
    }
    else
    {
        /* The arena is too small, and the message above reports it. */
    }

    p_room->len = b_ok ? (from + out_len) : from;

    return b_ok;
}

/* The start of the last page that take_page appended, given its length. */
static const uint8_t *page_at(const room_t *p_room, size_t out_len)
{
    return &p_room->p_bytes[p_room->len - out_len];
}

/*
 * Decodes PLAIN byte arrays from a buffer.
 *
 * Each value is a four byte little endian length and then that many bytes. The
 * output values point into the buffer.
 */
static bool plain_arrays(const uint8_t *p_in, size_t len,
                         parquet_string_t *p_out, size_t want, size_t *p_got)
{
    size_t at = 0U;
    size_t taken = 0U;
    bool b_ok = true;

    while (b_ok && (taken < want) && (at < len))
    {
        uint32_t size = 0U;

        if ((len - at) < 4U)
        {
            b_ok = false;
        }
        else
        {
            size = (uint32_t)p_in[at] | ((uint32_t)p_in[at + 1U] << 8) |
                   ((uint32_t)p_in[at + 2U] << 16) |
                   ((uint32_t)p_in[at + 3U] << 24);
            at += 4U;
            b_ok = ((size_t)size <= (len - at));
        }

        if (b_ok)
        {
            p_out[taken].p_bytes = &p_in[at];
            p_out[taken].len = (size_t)size;
            at += (size_t)size;
            taken++;
        }
    }

    *p_got = taken;

    return b_ok;
}

/* Keeps the dictionary of the current chunk, borrowed from its own page. */
static bool take_dictionary(parquet_file_t *p_file, size_t values)
{
    bool b_ok = true;

    if (p_file->dictionary_cap < values)
    {
        parquet_string_t *p_new = realloc(p_file->p_dictionary,
                                          values * sizeof(parquet_string_t));

        b_ok = (NULL != p_new);

        if (b_ok)
        {
            p_file->p_dictionary = p_new;
            p_file->dictionary_cap = values;
        }
    }

    if (b_ok)
    {
        b_ok = plain_arrays(p_file->dictionary_page.p_bytes,
                            p_file->dictionary_page.len, p_file->p_dictionary,
                            values, &p_file->dictionary_values);
    }

    return b_ok;
}

/* A four byte little endian word, as used by the level sections of a page. */
static uint32_t word_at(const uint8_t *p_at)
{
    return (uint32_t)p_at[0] | ((uint32_t)p_at[1] << 8) |
           ((uint32_t)p_at[2] << 16) | ((uint32_t)p_at[3] << 24);
}

/* The location of a page's definition levels, and of its values. */
typedef struct
{
    const uint8_t *p_levels;
    size_t         levels_len;
    const uint8_t *p_values;
    size_t         values_len;
} split_t;

/*
 * Splits a version one data page into its levels and its values.
 *
 * The levels carry their own four byte length, and the whole page, with its
 * levels, was compressed as one unit.
 */
static bool split_v1(const uint8_t *p_page, size_t len, bool b_optional,
                     split_t *p_out)
{
    bool b_ok = true;

    p_out->p_levels = NULL;
    p_out->levels_len = 0U;
    p_out->p_values = p_page;
    p_out->values_len = len;

    if (b_optional)
    {
        b_ok = (4U <= len);

        if (b_ok)
        {
            uint32_t levels = word_at(p_page);

            b_ok = ((size_t)levels <= (len - 4U));

            if (b_ok)
            {
                p_out->p_levels = &p_page[4];
                p_out->levels_len = (size_t)levels;
                p_out->p_values = &p_page[4U + levels];
                p_out->values_len = len - 4U - (size_t)levels;
            }
        }
    }

    return b_ok;
}

/*
 * Decodes the values of one data page into the output.
 *
 * A definition level of DEFINED means that the row holds a value, and a lower
 * level means that the row is null. A null takes a slot in the output, but it
 * does not use bytes from the page. The values are either stored in full, or
 * they are indices into the chunk's dictionary.
 */
static bool page_values(parquet_file_t *p_file, const page_head_t *p_head,
                        const split_t *p_split, bool b_optional,
                        parquet_string_t *p_out, size_t cap, size_t *p_got)
{
    rle_reader_t levels;
    rle_reader_t indices;
    parquet_string_t *p_plain = NULL;
    size_t plain_values = 0U;
    size_t taken = 0U;
    size_t from_page = 0U;
    bool b_dictionary = (META_ENC_RLE_DICTIONARY == p_head->encoding) ||
                        (META_ENC_PLAIN_DICTIONARY == p_head->encoding);
    bool b_ok = true;

    rle_open(&levels, p_split->p_levels, p_split->levels_len,
             b_optional ? 1U : 0U);

    if (b_dictionary)
    {
        /*
         * The first byte of the values is the bit width of the indices, and the
         * encoded runs start after it.
         */
        b_ok = (0U < p_split->values_len);

        if (b_ok)
        {
            rle_open(&indices, &p_split->p_values[1], p_split->values_len - 1U,
                     (uint32_t)p_split->p_values[0]);
        }
    }
    else if (META_ENC_PLAIN == p_head->encoding)
    {
        /* CAST: a page value count is positive, checked by the caller. */
        p_plain = calloc((size_t)p_head->num_values + 1U,
                         sizeof(parquet_string_t));
        b_ok = (NULL != p_plain) &&
               plain_arrays(p_split->p_values, p_split->values_len, p_plain,
                            (size_t)p_head->num_values, &plain_values);
    }
    else
    {
        (void)fprintf(stderr, "the reader decodes PLAIN and "
                              "RLE_DICTIONARY pages, and this page uses %s\n",
                      meta_encoding_name(p_head->encoding));
        b_ok = false;
    }

    while (b_ok && (taken < (size_t)p_head->num_values) && (taken < cap))
    {
        uint32_t level = DEFINED;

        if (b_optional)
        {
            b_ok = rle_next(&levels, &level);
        }

        if (b_ok && (DEFINED > level))
        {
            p_out[taken].p_bytes = NULL;
            p_out[taken].len = 0U;
        }
        else if (b_ok && b_dictionary)
        {
            uint32_t index = 0U;

            b_ok = rle_next(&indices, &index) &&
                   ((size_t)index < p_file->dictionary_values);

            if (b_ok)
            {
                p_out[taken] = p_file->p_dictionary[index];
            }
        }
        else if (b_ok)
        {
            b_ok = (from_page < plain_values);

            if (b_ok)
            {
                p_out[taken] = p_plain[from_page];
                from_page++;
            }
        }
        else
        {
            /* The level could not be read, and b_ok already says so. */
        }

        taken += (b_ok ? 1U : 0U);
    }

    free(p_plain);
    *p_got = taken;

    return b_ok;
}

/* Splits a version two data page, whose levels are stored uncompressed. */
static bool split_v2(const page_head_t *p_head, const uint8_t *p_raw,
                     size_t raw_len, room_t *p_room, int32_t codec,
                     split_t *p_out)
{
    /* CAST: both lengths are checked against the page size below. */
    size_t levels = (size_t)p_head->definition_levels_bytes +
                    (size_t)p_head->repetition_levels_bytes;
    bool b_ok = ((0 <= p_head->definition_levels_bytes) &&
                 (0 <= p_head->repetition_levels_bytes) &&
                 (levels <= raw_len));

    if (b_ok)
    {
        size_t want = (size_t)p_head->uncompressed_size - levels;

        p_out->p_levels = &p_raw[p_head->repetition_levels_bytes];
        p_out->levels_len = (size_t)p_head->definition_levels_bytes;

        if (p_head->b_compressed)
        {
            b_ok = take_page(p_room, &p_raw[levels], raw_len - levels, want,
                             codec);
            p_out->p_values = page_at(p_room, want);
            p_out->values_len = want;
        }
        else
        {
            p_out->p_values = &p_raw[levels];
            p_out->values_len = raw_len - levels;
        }
    }

    return b_ok;
}

/* The state that one chunk read needs, so that the walk takes one argument. */
typedef struct
{
    parquet_file_t   *p_file;
    const meta_chunk_t *p_chunk;
    bool              b_optional;
    parquet_string_t *p_out;
    size_t            cap;
    size_t            taken;
} walk_t;

/* Decodes one page of any kind into the output. */
static bool one_page(walk_t *p_walk, const page_head_t *p_head,
                     const uint8_t *p_raw)
{
    split_t split;
    size_t got = 0U;
    bool b_ok = true;

    (void)memset(&split, 0, sizeof(split));

    if (PAGE_DICTIONARY == p_head->type)
    {
        p_walk->p_file->dictionary_page.len = 0U;
        b_ok = room_for(&p_walk->p_file->dictionary_page,
                        (size_t)p_head->uncompressed_size + 1U) &&
               take_page(&p_walk->p_file->dictionary_page, p_raw,
                         (size_t)p_head->compressed_size,
                         (size_t)p_head->uncompressed_size,
                         p_walk->p_chunk->codec) &&
               take_dictionary(p_walk->p_file, (size_t)p_head->num_values);
    }
    else if (PAGE_DATA == p_head->type)
    {
        b_ok = take_page(&p_walk->p_file->page, p_raw,
                         (size_t)p_head->compressed_size,
                         (size_t)p_head->uncompressed_size,
                         p_walk->p_chunk->codec) &&
               split_v1(page_at(&p_walk->p_file->page,
                                (size_t)p_head->uncompressed_size),
                        (size_t)p_head->uncompressed_size,
                        p_walk->b_optional, &split) &&
               page_values(p_walk->p_file, p_head, &split, p_walk->b_optional,
                           &p_walk->p_out[p_walk->taken],
                           p_walk->cap - p_walk->taken, &got);
        p_walk->taken += got;
    }
    else if (PAGE_DATA_V2 == p_head->type)
    {
        b_ok = split_v2(p_head, p_raw, (size_t)p_head->compressed_size,
                        &p_walk->p_file->page, p_walk->p_chunk->codec,
                        &split) &&
               page_values(p_walk->p_file, p_head, &split, p_walk->b_optional,
                           &p_walk->p_out[p_walk->taken],
                           p_walk->cap - p_walk->taken, &got);
        p_walk->taken += got;
    }
    else
    {
        /* An index page does not carry values, so the walk skips it. */
        b_ok = (PAGE_INDEX == p_head->type);
    }

    return b_ok;
}

parquet_file_t *parquet_open(const uint8_t *p_bytes, size_t len)
{
    parquet_file_t *p_file = calloc(1U, sizeof(parquet_file_t));

    if (NULL != p_file)
    {
        p_file->p_file = p_bytes;
        p_file->len = len;

        if (!meta_read(p_bytes, len, &p_file->meta))
        {
            free(p_file);
            p_file = NULL;
        }
    }

    return p_file;
}

void parquet_close(parquet_file_t *p_file)
{
    if (NULL != p_file)
    {
        meta_free(&p_file->meta);
        room_free(&p_file->page);
        room_free(&p_file->dictionary_page);
        free(p_file->p_dictionary);
        free(p_file);
    }

    return;
}

int64_t parquet_rows(const parquet_file_t *p_file)
{
    return p_file->meta.num_rows;
}

size_t parquet_row_groups(const parquet_file_t *p_file)
{
    return p_file->meta.row_groups;
}

size_t parquet_row_group_rows(const parquet_file_t *p_file, size_t row_group)
{
    const meta_chunk_t *p_chunk = meta_chunk_of(&p_file->meta, row_group, 0U);
    size_t out = 0U;

    if ((NULL != p_chunk) && (0 < p_chunk->num_values))
    {
        /* CAST: a value count in this format is a positive 64 bit number. */
        out = (size_t)p_chunk->num_values;
    }

    return out;
}

bool parquet_has_column(const parquet_file_t *p_file, const char *p_name)
{
    return (meta_field_of(&p_file->meta, p_name) < p_file->meta.fields);
}

/* Refuses a chunk that the reader cannot decode, and names the cause. */
static bool chunk_is_readable(const meta_chunk_t *p_chunk, const char *p_name,
                              bool b_nested, bool b_repeated)
{
    /* The known encodings, as the bit set that the footer reader builds. */
    const uint32_t known = ((uint32_t)1U << META_ENC_PLAIN) |
                           ((uint32_t)1U << META_ENC_RLE) |
                           ((uint32_t)1U << META_ENC_BIT_PACKED) |
                           ((uint32_t)1U << META_ENC_PLAIN_DICTIONARY) |
                           ((uint32_t)1U << META_ENC_RLE_DICTIONARY);
    uint32_t unknown = p_chunk->encodings & ~known;
    bool b_ok = (META_TYPE_BYTE_ARRAY == p_chunk->type);

    if (!b_ok)
    {
        (void)fprintf(stderr, "the column %s is not a string column\n",
                      p_name);
    }
    else if (b_repeated)
    {
        (void)fprintf(stderr, "the column %s is repeated, so its rows need "
                              "repetition levels, which the reader does not "
                              "decode\n", p_name);
        b_ok = false;
    }
    else if (b_nested)
    {
        (void)fprintf(stderr, "the column %s sits inside a group, so its rows "
                              "need repetition levels, which the reader "
                              "does not decode\n", p_name);
        b_ok = false;
    }
    else if (0U != unknown)
    {
        uint32_t bit = 0U;

        for (bit = 0U; bit < 32U; bit++)
        {
            if (0U != (unknown & ((uint32_t)1U << bit)))
            {
                (void)fprintf(stderr, "the column %s uses %s, which this does "
                                      "not decode\n", p_name,
                              /* CAST: the bit index is the encoding number. */
                              meta_encoding_name((int32_t)bit));
            }
        }

        b_ok = false;
    }
    else
    {
        /* The reader decodes every encoding that the chunk declares. */
    }

    return b_ok;
}

bool parquet_read_strings(parquet_file_t *p_file, const char *p_name,
                          size_t row_group, parquet_string_t *p_out,
                          size_t cap, size_t *p_count)
{
    walk_t walk;
    size_t field = meta_field_of(&p_file->meta, p_name);
    const meta_chunk_t *p_chunk = meta_chunk_of(&p_file->meta, row_group,
                                                field);
    size_t at = 0U;
    size_t end = 0U;
    bool b_ok = (NULL != p_chunk);

    *p_count = 0U;

    if (!b_ok)
    {
        (void)fprintf(stderr, "the column %s is absent from row group %zu\n",
                      p_name, row_group);
    }
    else
    {
        b_ok = chunk_is_readable(p_chunk, p_name,
                                 p_file->meta.p_field[field].b_nested,
                                 (2 == p_file->meta.p_field[field].repetition));
    }

    if (b_ok)
    {
        /*
         * A chunk starts at its dictionary page when it has one. The dictionary
         * is written before the data, so a start at the data page offset would
         * skip the dictionary.
         */
        int64_t from = (0 != p_chunk->dictionary_page_offset)
                       ? p_chunk->dictionary_page_offset
                       : p_chunk->data_page_offset;

        b_ok = (0 <= from) && ((uint64_t)from < (uint64_t)p_file->len);
        /* CAST: bounded by the file length, which is a size_t. */
        at = b_ok ? (size_t)from : 0U;
        end = at + (size_t)p_chunk->total_compressed_size;
        b_ok = b_ok && (end <= p_file->len);
    }

    if (b_ok)
    {
        /*
         * The arena holds the whole chunk, and it is allocated before the walk.
         * A value of a PLAIN page is borrowed from this memory, so the memory
         * must not move while later pages are appended to it.
         */
        p_file->page.len = 0U;
        b_ok = (0 <= p_chunk->total_uncompressed_size) &&
               room_for(&p_file->page,
                        (size_t)p_chunk->total_uncompressed_size + 1U);
    }

    (void)memset(&walk, 0, sizeof(walk));
    walk.p_file = p_file;
    walk.p_chunk = p_chunk;
    walk.p_out = p_out;
    walk.cap = cap;
    walk.b_optional = b_ok &&
                      (1 == p_file->meta.p_field[field].repetition);
    p_file->dictionary_values = 0U;

    while (b_ok && (at < end) &&
           (walk.taken < (size_t)p_chunk->num_values) && (walk.taken < cap))
    {
        page_head_t head;
        size_t head_len = 0U;

        b_ok = read_page_head(&p_file->p_file[at], end - at, &head, &head_len);
        at += head_len;
        b_ok = b_ok && ((size_t)head.compressed_size <= (end - at));

        if (b_ok)
        {
            b_ok = one_page(&walk, &head, &p_file->p_file[at]);
            at += (size_t)head.compressed_size;
        }
    }

    *p_count = walk.taken;

    return b_ok;
}
