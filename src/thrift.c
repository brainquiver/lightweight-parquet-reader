/* thrift.h explains the scope and why the field delta needs a stack. */
#include "thrift.h"

/* A varint holds at most ten bytes at 64 bits, seven bits to a byte. */
#define VARINT_MAX_BYTES 10U

/* The low nibble of a field header or a list header is the element type. */
#define NIBBLE_MASK 0x0FU

/* One byte, or zero at the end of the buffer, where it also sets the fault. */
static uint8_t next_byte(thrift_reader_t *p_reader)
{
    uint8_t out = 0U;

    if (p_reader->at < p_reader->len)
    {
        out = p_reader->p_data[p_reader->at];
        p_reader->at++;
    }
    else
    {
        p_reader->b_bad = true;
    }

    return out;
}

void thrift_open(thrift_reader_t *p_reader, const uint8_t *p_data, size_t len)
{
    size_t i = 0U;

    p_reader->p_data = p_data;
    p_reader->len = len;
    p_reader->at = 0U;
    p_reader->b_bad = false;
    p_reader->depth = 0U;

    for (i = 0U; i < THRIFT_MAX_DEPTH; i++)
    {
        p_reader->p_last_id[i] = 0;
    }

    return;
}

bool thrift_ok(const thrift_reader_t *p_reader)
{
    return !p_reader->b_bad;
}

uint64_t thrift_varint(thrift_reader_t *p_reader)
{
    uint64_t out = 0U;
    uint32_t shift = 0U;
    size_t taken = 0U;
    bool b_more = true;

    while (b_more && !p_reader->b_bad)
    {
        uint8_t byte = next_byte(p_reader);

        taken++;

        if (VARINT_MAX_BYTES < taken)
        {
            p_reader->b_bad = true;
        }
        else
        {
            out |= ((uint64_t)(byte & 0x7FU)) << shift;
            shift += 7U;
            b_more = (0U != (byte & 0x80U));
        }
    }

    return p_reader->b_bad ? 0U : out;
}

int64_t thrift_zigzag(thrift_reader_t *p_reader)
{
    uint64_t raw = thrift_varint(p_reader);
    /*
     * The protocol writes a signed value with the sign in the low bit. The
     * magnitude is recovered with an unsigned shift, because a shift of a
     * signed value is not portable.
     */
    uint64_t magnitude = raw >> 1;
    int64_t out = 0;

    if (0U == (raw & 1U))
    {
        /*
         * CAST: the low bit was zero and the shift removed it, so the value is
         * at most half of the unsigned range and fits a signed 64 bit integer.
         */
        out = (int64_t)magnitude;
    }
    else
    {
        /*
         * CAST: the same bound holds, and the largest magnitude gives the most
         * negative value that the type holds, which is exactly the range that
         * the protocol encodes.
         */
        out = (-(int64_t)magnitude) - 1;
    }

    return out;
}

void thrift_struct_begin(thrift_reader_t *p_reader)
{
    if ((p_reader->depth + 1U) < THRIFT_MAX_DEPTH)
    {
        p_reader->depth++;
        p_reader->p_last_id[p_reader->depth] = 0;
    }
    else
    {
        /* A document that claims to nest without limit is refused. */
        p_reader->b_bad = true;
    }

    return;
}

void thrift_struct_end(thrift_reader_t *p_reader)
{
    if (0U < p_reader->depth)
    {
        p_reader->depth--;
    }
    else
    {
        p_reader->b_bad = true;
    }

    return;
}

bool thrift_field(thrift_reader_t *p_reader, uint8_t *p_type, int16_t *p_id)
{
    uint8_t head = 0U;
    uint8_t delta = 0U;
    bool b_more = false;

    *p_type = THRIFT_STOP;
    *p_id = 0;
    head = next_byte(p_reader);
    delta = (uint8_t)((head >> 4) & NIBBLE_MASK);
    *p_type = (uint8_t)(head & NIBBLE_MASK);

    if (p_reader->b_bad || (THRIFT_STOP == *p_type))
    {
        b_more = false;
    }
    else if (0U == delta)
    {
        int64_t absolute = thrift_zigzag(p_reader);

        /*
         * CAST: a parquet field identifier is a small positive number, and the
         * protocol defines the field identifier as a 16 bit value. A value
         * outside that range means a malformed document.
         */
        if ((absolute < INT16_MIN) || (absolute > INT16_MAX))
        {
            p_reader->b_bad = true;
        }
        else
        {
            *p_id = (int16_t)absolute;
            b_more = true;
        }
    }
    else
    {
        /*
         * CAST: the delta is four bits, so the sum of it and a 16 bit
         * identifier is checked against the range before it is narrowed.
         */
        int32_t sum = (int32_t)p_reader->p_last_id[p_reader->depth] +
                      (int32_t)delta;

        if (sum > INT16_MAX)
        {
            p_reader->b_bad = true;
        }
        else
        {
            *p_id = (int16_t)sum;
            b_more = true;
        }
    }

    if (b_more)
    {
        p_reader->p_last_id[p_reader->depth] = *p_id;
    }

    return b_more && !p_reader->b_bad;
}

bool thrift_list(thrift_reader_t *p_reader, uint8_t *p_type, uint32_t *p_count)
{
    uint8_t head = next_byte(p_reader);
    uint32_t count = (uint32_t)((head >> 4) & NIBBLE_MASK);

    *p_type = (uint8_t)(head & NIBBLE_MASK);

    if (NIBBLE_MASK == count)
    {
        uint64_t wide = thrift_varint(p_reader);

        /*
         * The bytes left in the buffer bound the count. The smallest element
         * that the protocol writes is one byte, so a list cannot hold more
         * elements than the bytes left. A bound on the whole buffer accepts a
         * count that the bytes left cannot supply.
         */
        if (wide > (uint64_t)(p_reader->len - p_reader->at))
        {
            p_reader->b_bad = true;
        }

        /* CAST: bounded above by the buffer length, which is a size_t. */
        count = (uint32_t)(p_reader->b_bad ? 0U : wide);
    }

    *p_count = count;

    return !p_reader->b_bad;
}

bool thrift_binary(thrift_reader_t *p_reader, const uint8_t **pp_out,
                   size_t *p_len)
{
    uint64_t len = thrift_varint(p_reader);

    *pp_out = NULL;
    *p_len = 0U;

    if (!p_reader->b_bad)
    {
        /* The run must lie inside the buffer, and the test cannot overflow. */
        if (len > (uint64_t)(p_reader->len - p_reader->at))
        {
            p_reader->b_bad = true;
        }
        else
        {
            /* CAST: bounded above by the bytes left, which is a size_t. */
            *p_len = (size_t)len;
            *pp_out = &p_reader->p_data[p_reader->at];
            p_reader->at += *p_len;
        }
    }

    return !p_reader->b_bad;
}

/* Skips the fields of a nested structure. */
static void skip_struct(thrift_reader_t *p_reader)
{
    uint8_t type = THRIFT_STOP;
    int16_t id = 0;

    thrift_struct_begin(p_reader);

    while (thrift_field(p_reader, &type, &id))
    {
        thrift_skip(p_reader, type);
    }

    thrift_struct_end(p_reader);

    return;
}

/* Skips the elements of a list or a set. */
static void skip_list(thrift_reader_t *p_reader)
{
    uint8_t type = THRIFT_STOP;
    uint32_t count = 0U;
    uint32_t i = 0U;

    if (thrift_list(p_reader, &type, &count))
    {
        for (i = 0U; (i < count) && !p_reader->b_bad; i++)
        {
            thrift_skip(p_reader, type);
        }
    }

    return;
}

/*
 * Skips the entries of a map.
 *
 * A map writes its size first and the two element types after it, and an empty
 * map omits the type byte. A reader that always reads the type byte consumes
 * the first byte of whatever follows an empty map.
 */
static void skip_map(thrift_reader_t *p_reader)
{
    uint64_t count = thrift_varint(p_reader);
    uint32_t i = 0U;

    if (!p_reader->b_bad && (0U != count))
    {
        uint8_t types = next_byte(p_reader);
        uint8_t key_type = (uint8_t)((types >> 4) & NIBBLE_MASK);
        uint8_t value_type = (uint8_t)(types & NIBBLE_MASK);

        if (count > (uint64_t)p_reader->len)
        {
            p_reader->b_bad = true;
        }

        for (i = 0U; ((uint64_t)i < count) && !p_reader->b_bad; i++)
        {
            thrift_skip(p_reader, key_type);
            thrift_skip(p_reader, value_type);
        }
    }

    return;
}

void thrift_skip(thrift_reader_t *p_reader, uint8_t type)
{
    const uint8_t *p_bytes = NULL;
    size_t len = 0U;

    switch (type)
    {
        case THRIFT_TRUE:
        case THRIFT_FALSE:
            /* The value is the type, so the field header is the whole field. */
            break;

        case THRIFT_BYTE:
            (void)next_byte(p_reader);
            break;

        case THRIFT_I16:
        case THRIFT_I32:
        case THRIFT_I64:
            (void)thrift_zigzag(p_reader);
            break;

        case THRIFT_DOUBLE:
            /* Always eight bytes, little endian. */
            if (8U > (p_reader->len - p_reader->at))
            {
                p_reader->b_bad = true;
            }
            else
            {
                p_reader->at += 8U;
            }

            break;

        case THRIFT_BINARY:
            (void)thrift_binary(p_reader, &p_bytes, &len);
            break;

        case THRIFT_LIST:
        case THRIFT_SET:
            skip_list(p_reader);
            break;

        case THRIFT_MAP:
            skip_map(p_reader);
            break;

        case THRIFT_STRUCT:
            skip_struct(p_reader);
            break;

        default:
            /* The reader cannot skip an unknown type, so the walk stops. */
            p_reader->b_bad = true;
            break;
    }

    return;
}
