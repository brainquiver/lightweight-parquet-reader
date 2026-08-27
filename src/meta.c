/* meta.h lists the fields of a parquet footer that are kept and skipped. */
#include "meta.h"
#include "thrift.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The four bytes at the start and at the end of a parquet file. */
#define MAGIC "PAR1"
#define MAGIC_LEN 4U
/* The footer length is a four byte little endian word before the last magic. */
#define TAIL_LEN 8U

/* The largest field count and row group count that the reader accepts. */
#define MAX_FIELDS 4096U
#define MAX_ROW_GROUPS 65536U

const char *meta_codec_name(int32_t codec)
{
    const char *p_name = "an unknown codec";

    switch (codec)
    {
        case META_CODEC_UNCOMPRESSED: p_name = "UNCOMPRESSED"; break;
        case META_CODEC_SNAPPY:       p_name = "SNAPPY";       break;
        case META_CODEC_GZIP:         p_name = "GZIP";         break;
        case META_CODEC_LZO:          p_name = "LZO";          break;
        case META_CODEC_BROTLI:       p_name = "BROTLI";       break;
        case META_CODEC_LZ4:          p_name = "LZ4";          break;
        case META_CODEC_ZSTD:         p_name = "ZSTD";         break;
        case META_CODEC_LZ4_RAW:      p_name = "LZ4_RAW";      break;
        default:                                               break;
    }

    return p_name;
}

const char *meta_encoding_name(int32_t encoding)
{
    const char *p_name = "an unknown encoding";

    switch (encoding)
    {
        case META_ENC_PLAIN:            p_name = "PLAIN";            break;
        case META_ENC_PLAIN_DICTIONARY: p_name = "PLAIN_DICTIONARY"; break;
        case META_ENC_RLE:              p_name = "RLE";              break;
        case META_ENC_BIT_PACKED:       p_name = "BIT_PACKED";       break;
        case META_ENC_DELTA_BINARY:     p_name = "DELTA_BINARY_PACKED"; break;
        case META_ENC_DELTA_LEN_ARRAY:
            p_name = "DELTA_LENGTH_BYTE_ARRAY";
            break;
        case META_ENC_DELTA_ARRAY:      p_name = "DELTA_BYTE_ARRAY";  break;
        case META_ENC_RLE_DICTIONARY:   p_name = "RLE_DICTIONARY";    break;
        case META_ENC_BYTE_STREAM:      p_name = "BYTE_STREAM_SPLIT"; break;
        default:                                                      break;
    }

    return p_name;
}

/* Copy a borrowed name, and refuse a name that does not fit. */
static bool take_name(char *p_out, const uint8_t *p_bytes, size_t len)
{
    bool b_ok = (len < META_NAME_MAX);

    if (b_ok)
    {
        (void)memcpy(p_out, p_bytes, len);
        p_out[len] = '\0';
    }

    return b_ok;
}

/*
 * One SchemaElement.
 *
 * Field 1 is the physical type. A group does not have it, which is how the
 * reader tells the root of the schema from a leaf. Field 4 is the name and
 * field 3 is the repetition.
 */
static bool read_schema_element(thrift_reader_t *p_reader, meta_field_t *p_out,
                                int32_t *p_children)
{
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;
    bool b_named = true;

    p_out->type = -1;
    p_out->repetition = 0;
    p_out->b_nested = false;
    p_out->p_name[0] = '\0';
    *p_children = 0;
    thrift_struct_begin(p_reader);

    while (thrift_field(p_reader, &type, &id))
    {
        if (1 == id)
        {
            /* CAST: a parquet physical type is one of eight small values. */
            p_out->type = (int32_t)thrift_zigzag(p_reader);
        }
        else if (3 == id)
        {
            /* CAST: a repetition is one of three small values. */
            p_out->repetition = (int32_t)thrift_zigzag(p_reader);
        }
        else if (4 == id)
        {
            const uint8_t *p_bytes = NULL;
            size_t len = 0U;

            if (thrift_binary(p_reader, &p_bytes, &len) &&
                !take_name(p_out->p_name, p_bytes, len))
            {
                (void)fprintf(stderr, "a column name of %zu bytes is longer "
                                      "than this reader keeps\n", len);
                b_named = false;
            }
        }
        else if (5 == id)
        {
            /* CAST: a child count is a small positive number in a schema. */
            *p_children = (int32_t)thrift_zigzag(p_reader);
        }
        else
        {
            thrift_skip(p_reader, type);
        }
    }

    thrift_struct_end(p_reader);

    return b_named && thrift_ok(p_reader);
}

/* The list of encodings a chunk declares, as one bit for each. */
static uint32_t read_encodings(thrift_reader_t *p_reader)
{
    uint8_t element = THRIFT_STOP;
    uint32_t count = 0U;
    uint32_t out = 0U;
    uint32_t i = 0U;

    if (thrift_list(p_reader, &element, &count))
    {
        for (i = 0U; (i < count) && thrift_ok(p_reader); i++)
        {
            int64_t value = thrift_zigzag(p_reader);

            /* An encoding outside the defined range does not set a bit, and
               a page that declares it is refused later, by name. */
            if ((0 <= value) && (31 > value))
            {
                out |= (uint32_t)1U << (uint32_t)value;
            }
        }
    }

    return out;
}

/*
 * One ColumnMetaData, which gives the location of a column chunk's bytes.
 *
 * The reader skips field 3, the path in the schema. The schema leaves keep
 * the order of the column chunks, so a field index is already a chunk index.
 */
static bool read_column_meta(thrift_reader_t *p_reader, meta_chunk_t *p_out)
{
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;

    thrift_struct_begin(p_reader);

    while (thrift_field(p_reader, &type, &id))
    {
        if (1 == id)
        {
            /* CAST: a physical type is one of eight small values. */
            p_out->type = (int32_t)thrift_zigzag(p_reader);
        }
        else if (2 == id)
        {
            p_out->encodings = read_encodings(p_reader);
        }
        else if (4 == id)
        {
            /* CAST: a codec is one of eight small values. */
            p_out->codec = (int32_t)thrift_zigzag(p_reader);
        }
        else if (5 == id)
        {
            p_out->num_values = thrift_zigzag(p_reader);
        }
        else if (6 == id)
        {
            p_out->total_uncompressed_size = thrift_zigzag(p_reader);
        }
        else if (7 == id)
        {
            p_out->total_compressed_size = thrift_zigzag(p_reader);
        }
        else if (9 == id)
        {
            p_out->data_page_offset = thrift_zigzag(p_reader);
        }
        else if (11 == id)
        {
            p_out->dictionary_page_offset = thrift_zigzag(p_reader);
        }
        else
        {
            thrift_skip(p_reader, type);
        }
    }

    thrift_struct_end(p_reader);

    return thrift_ok(p_reader);
}

/* One ColumnChunk. Field 3 holds the metadata that the reader needs. */
static bool read_column_chunk(thrift_reader_t *p_reader, meta_chunk_t *p_out)
{
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;
    bool b_ok = true;

    (void)memset(p_out, 0, sizeof(*p_out));
    p_out->type = -1;
    thrift_struct_begin(p_reader);

    while (b_ok && thrift_field(p_reader, &type, &id))
    {
        if (3 == id)
        {
            b_ok = read_column_meta(p_reader, p_out);
        }
        else
        {
            thrift_skip(p_reader, type);
        }
    }

    thrift_struct_end(p_reader);

    return b_ok && thrift_ok(p_reader);
}

/* The state of one pass over the footer, so the walk takes one argument. */
/* The deepest schema tree that the reader accepts. A deeper one is refused. */
#define SCHEMA_MAX_DEPTH 32U

typedef struct
{
    meta_file_t *p_out;
    /* How many children each open group still owes, by depth. */
    int32_t      owed[SCHEMA_MAX_DEPTH];
    size_t       depth;
    size_t       fields_seen;
    size_t       groups_seen;
    size_t       chunks_seen;
    bool         b_ok;
} build_t;

/* The schema list. Its first element is the root, which is not a column. */
static void read_schema(thrift_reader_t *p_reader, build_t *p_build)
{
    uint8_t element = THRIFT_STOP;
    uint32_t count = 0U;
    uint32_t i = 0U;

    if (!thrift_list(p_reader, &element, &count) || (MAX_FIELDS < count))
    {
        p_build->b_ok = false;
        return;
    }

    p_build->p_out->p_field = calloc(count, sizeof(meta_field_t));
    p_build->b_ok = (NULL != p_build->p_out->p_field);

    /*
     * The schema is a tree, written in order, and each element gives the
     * number of children that follow it. A countdown of those children tells
     * a leaf from a group, and a leaf directly under the root from a leaf
     * inside a list. Only the leaves have column chunks, and the chunks follow
     * in this order.
     */
    for (i = 0U; (i < count) && p_build->b_ok; i++)
    {
        meta_field_t one;
        int32_t children = 0;

        p_build->b_ok = read_schema_element(p_reader, &one, &children);

        if (p_build->b_ok && (0U == i))
        {
            /* The root names the file and is not a column. */
            p_build->owed[0] = children;
            p_build->depth = 1U;
        }
        else if (p_build->b_ok)
        {
            /* This element is one of the children that the open group owes. */
            while ((1U < p_build->depth) && (0 == p_build->owed[
                       p_build->depth - 1U]))
            {
                p_build->depth--;
            }

            if (0U != p_build->depth)
            {
                p_build->owed[p_build->depth - 1U]--;
            }

            if (0 < children)
            {
                p_build->b_ok = (SCHEMA_MAX_DEPTH > p_build->depth);

                if (p_build->b_ok)
                {
                    p_build->owed[p_build->depth] = children;
                    p_build->depth++;
                }
            }
            else
            {
                one.b_nested = (1U < p_build->depth);
                p_build->p_out->p_field[p_build->fields_seen] = one;
                p_build->fields_seen++;
            }
        }
        else
        {
            /* The element failed to read, and b_ok already records it. */
        }
    }

    p_build->p_out->fields = p_build->fields_seen;

    return;
}

/* One RowGroup. Field 1 is its list of column chunks. */
static void read_row_group(thrift_reader_t *p_reader, build_t *p_build)
{
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;

    thrift_struct_begin(p_reader);

    while (p_build->b_ok && thrift_field(p_reader, &type, &id))
    {
        if (1 == id)
        {
            uint8_t element = THRIFT_STOP;
            uint32_t count = 0U;
            uint32_t i = 0U;

            p_build->b_ok = thrift_list(p_reader, &element, &count);

            if (p_build->b_ok &&
                (count != (uint32_t)p_build->p_out->fields))
            {
                (void)fprintf(stderr, "the schema has %zu leaves and a row "
                                      "group holds %u column chunks, and this "
                                      "reader does not support that shape\n",
                              p_build->p_out->fields, count);
                p_build->b_ok = false;
            }

            for (i = 0U; (i < count) && p_build->b_ok; i++)
            {
                p_build->b_ok = read_column_chunk(
                    p_reader,
                    &p_build->p_out->p_chunk[p_build->chunks_seen]);
                p_build->chunks_seen++;
            }
        }
        else
        {
            thrift_skip(p_reader, type);
        }
    }

    thrift_struct_end(p_reader);
    p_build->groups_seen++;

    return;
}

/* The row groups, read after the schema gives the number of columns. */
static void read_row_groups(thrift_reader_t *p_reader, build_t *p_build)
{
    uint8_t element = THRIFT_STOP;
    uint32_t count = 0U;
    uint32_t i = 0U;

    if (!thrift_list(p_reader, &element, &count) || (MAX_ROW_GROUPS < count))
    {
        p_build->b_ok = false;
        return;
    }

    p_build->p_out->p_chunk = calloc((size_t)count * p_build->p_out->fields,
                                     sizeof(meta_chunk_t));
    p_build->b_ok = (NULL != p_build->p_out->p_chunk) ||
                    (0U == (count * p_build->p_out->fields));

    for (i = 0U; (i < count) && p_build->b_ok; i++)
    {
        read_row_group(p_reader, p_build);
    }

    p_build->p_out->row_groups = p_build->groups_seen;

    return;
}

/* The four byte little endian word before the last magic. */
static uint32_t footer_length(const uint8_t *p_file, size_t len)
{
    const uint8_t *p_at = &p_file[len - TAIL_LEN];

    return (uint32_t)p_at[0] | ((uint32_t)p_at[1] << 8) |
           ((uint32_t)p_at[2] << 16) | ((uint32_t)p_at[3] << 24);
}

/* True when the file opens and closes with the parquet magic. */
static bool magic_holds(const uint8_t *p_file, size_t len)
{
    bool b_ok = (len > (MAGIC_LEN + TAIL_LEN));

    if (b_ok)
    {
        b_ok = (0 == memcmp(p_file, MAGIC, MAGIC_LEN)) &&
               (0 == memcmp(&p_file[len - MAGIC_LEN], MAGIC, MAGIC_LEN));
    }

    if (!b_ok)
    {
        (void)fprintf(stderr, "the file does not open and close with PAR1, "
                              "so it is not a parquet file\n");
    }

    return b_ok;
}

bool meta_read(const uint8_t *p_file, size_t len, meta_file_t *p_out)
{
    thrift_reader_t reader;
    build_t build;
    uint32_t footer = 0U;
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;

    (void)memset(p_out, 0, sizeof(*p_out));
    (void)memset(&build, 0, sizeof(build));
    build.p_out = p_out;
    build.b_ok = magic_holds(p_file, len);

    if (build.b_ok)
    {
        footer = footer_length(p_file, len);
        build.b_ok = ((size_t)footer <= (len - MAGIC_LEN - TAIL_LEN));

        if (!build.b_ok)
        {
            (void)fprintf(stderr, "the footer claims %u bytes and the file "
                                  "holds %zu\n", footer, len);
        }
    }

    if (build.b_ok)
    {
        thrift_open(&reader, &p_file[(len - TAIL_LEN) - footer], footer);
        thrift_struct_begin(&reader);

        while (build.b_ok && thrift_field(&reader, &type, &id))
        {
            if (1 == id)
            {
                /* CAST: the format version is a small positive number. */
                p_out->version = (int32_t)thrift_zigzag(&reader);
            }
            else if (2 == id)
            {
                read_schema(&reader, &build);
            }
            else if (3 == id)
            {
                p_out->num_rows = thrift_zigzag(&reader);
            }
            else if (4 == id)
            {
                read_row_groups(&reader, &build);
            }
            else
            {
                thrift_skip(&reader, type);
            }
        }

        thrift_struct_end(&reader);
        build.b_ok = build.b_ok && thrift_ok(&reader);

        if (!build.b_ok)
        {
            (void)fprintf(stderr, "the footer does not parse as a thrift "
                                  "document\n");
        }
    }

    if (!build.b_ok)
    {
        meta_free(p_out);
    }

    return build.b_ok;
}

void meta_free(meta_file_t *p_meta)
{
    free(p_meta->p_field);
    free(p_meta->p_chunk);
    p_meta->p_field = NULL;
    p_meta->p_chunk = NULL;
    p_meta->fields = 0U;
    p_meta->row_groups = 0U;

    return;
}

size_t meta_field_of(const meta_file_t *p_meta, const char *p_name)
{
    size_t i = 0U;
    size_t found = p_meta->fields;

    for (i = 0U; (i < p_meta->fields) && (found == p_meta->fields); i++)
    {
        if (0 == strcmp(p_meta->p_field[i].p_name, p_name))
        {
            found = i;
        }
    }

    return found;
}

const meta_chunk_t *meta_chunk_of(const meta_file_t *p_meta, size_t row_group,
                                  size_t field)
{
    const meta_chunk_t *p_out = NULL;

    if ((row_group < p_meta->row_groups) && (field < p_meta->fields))
    {
        p_out = &p_meta->p_chunk[(row_group * p_meta->fields) + field];
    }

    return p_out;
}
