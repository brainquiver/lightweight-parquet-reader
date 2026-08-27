/*
 * thrift.h: the Thrift compact protocol, read only.
 *
 * Parquet keeps its file metadata and every page header in this encoding, so
 * every other read of a parquet file starts here. The protocol is small, and
 * this module reads only the part that parquet uses. That part is a varint, a
 * zigzag varint, a field header, a list header and a skip over a field of any
 * type.
 *
 * Scope.
 *
 * The module only reads, and it does not build a document in memory. The
 * metadata of a parquet file holds hundreds of fields, and the parquet reader
 * needs about fifteen. A caller therefore walks the fields that it needs and
 * skips the rest.
 *
 * Field delta.
 *
 * A field header carries the difference between its field identifier and the
 * previous one, and that difference restarts inside every nested structure. A
 * reader with a single last identifier reads the fields of an outer structure
 * with the identifiers of an inner one. The fault is silent: every field is
 * still a valid field, so every check passes, and the document means something
 * else. For this reason, the stack below keeps one last identifier per depth.
 *
 * Sticky errors.
 *
 * A malformed read sets a flag that stays set, and every later call returns
 * zero and does not read the buffer. A caller can therefore walk a whole
 * structure and test the flag once at the end.
 */
#ifndef THRIFT_H
#define THRIFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The field types of the compact protocol.
 *
 * A boolean carries its value in the type, so the protocol has two boolean
 * types, and a boolean field occupies only its field header.
 */
#define THRIFT_STOP    0x00U
#define THRIFT_TRUE    0x01U
#define THRIFT_FALSE   0x02U
#define THRIFT_BYTE    0x03U
#define THRIFT_I16     0x04U
#define THRIFT_I32     0x05U
#define THRIFT_I64     0x06U
#define THRIFT_DOUBLE  0x07U
#define THRIFT_BINARY  0x08U
#define THRIFT_LIST    0x09U
#define THRIFT_SET     0x0AU
#define THRIFT_MAP     0x0BU
#define THRIFT_STRUCT  0x0CU

/*
 * The maximum depth of a nested structure.
 *
 * Parquet metadata nests five deep at most, with a column chunk inside a row
 * group inside the file, and statistics inside the column. Sixteen leaves room
 * and bounds the recursion, because a file that claims to nest without limit
 * must be refused.
 */
#define THRIFT_MAX_DEPTH 16U

typedef struct
{
    const uint8_t *p_data;
    size_t         len;
    size_t         at;
    /* Sticky. Set by any read that runs past the end or breaks the shape. */
    bool           b_bad;
    /* The last field identifier seen at each depth, for the delta coding. */
    int16_t        p_last_id[THRIFT_MAX_DEPTH];
    size_t         depth;
} thrift_reader_t;

/* Points a reader at a buffer, which it borrows and never frees. */
void thrift_open(thrift_reader_t *p_reader, const uint8_t *p_data, size_t len);

/* True when every read so far was well formed. */
bool thrift_ok(const thrift_reader_t *p_reader);

/* An unsigned varint, up to 64 bits. Zero when the reader is already bad. */
uint64_t thrift_varint(thrift_reader_t *p_reader);

/* A signed varint in zigzag form, which is how I16 to I64 are written. */
int64_t thrift_zigzag(thrift_reader_t *p_reader);

/*
 * The next field of the current structure.
 *
 * Writes the type and the identifier, and returns false at the end of the
 * structure or on any fault. For a boolean field, the type is THRIFT_TRUE or
 * THRIFT_FALSE and carries the value. The function reads only the field header,
 * and the caller takes the value from the type.
 */
bool thrift_field(thrift_reader_t *p_reader, uint8_t *p_type, int16_t *p_id);

/* The start and end of a nested structure. Its field headers sit between. */
void thrift_struct_begin(thrift_reader_t *p_reader);
void thrift_struct_end(thrift_reader_t *p_reader);

/*
 * The header of a list, which gives the count and the type of its elements.
 *
 * A set has the same encoding, so this function also reads a set.
 */
bool thrift_list(thrift_reader_t *p_reader, uint8_t *p_type, uint32_t *p_count);

/*
 * A length prefixed run of bytes, borrowed from the buffer.
 *
 * The result points into the buffer, so the bytes live as long as the buffer.
 */
bool thrift_binary(thrift_reader_t *p_reader, const uint8_t **pp_out,
                   size_t *p_len);

/* A skip over a field of this type, whatever it holds. */
void thrift_skip(thrift_reader_t *p_reader, uint8_t type);

#endif /* THRIFT_H */
