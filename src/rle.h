/*
 * rle.h: the run length and bit packed hybrid encoding, read only.
 *
 * Parquet writes two things in this encoding: the definition levels that mark
 * the null values, and the dictionary indices of a dictionary encoded column.
 * Both are small unsigned integers, so both are written in as few bits as the
 * largest of them needs.
 *
 * Run shapes.
 *
 * The data is a sequence of runs, and each run starts with a varint header.
 * The low bit of that header gives the shape of the run.
 *
 *     header with the low bit set     a bit packed run.
 *                                     The rest is a count of groups, and each
 *                                     group holds eight values.
 *     header with the low bit clear   a repeated run.
 *                                     The rest is the count of values, and one
 *                                     value follows, in as many whole bytes as
 *                                     the bit width needs.
 *
 * Bit order.
 *
 * A bit packed run fills each byte from its lowest bit upwards, and a value
 * that does not fit continues in the next byte. The parquet specification
 * defines this order. A person usually draws a byte with its highest bit on
 * the left, so a decoder written from that picture reverses every value.
 *
 * Errors are sticky, as they are in the thrift reader, so a caller reads a
 * whole run of values and tests once at the end.
 */
#ifndef RLE_H
#define RLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const uint8_t *p_data;
    size_t         len;
    size_t         at;
    /* The bit inside byte p_data[at] where the next packed value starts. */
    uint32_t       bit_at;
    uint32_t       bit_width;
    /* How many values are left in the current run. */
    uint64_t       run_left;
    /* True when that run is bit packed, false when it repeats one value. */
    bool           b_packed;
    uint32_t       repeated;
    bool           b_bad;
} rle_reader_t;

/*
 * Point a reader at the encoded bytes. The buffer is borrowed.
 *
 * A bit width of zero is legal and means that every value is zero. A column
 * without null values has definition levels that need zero bits.
 */
void rle_open(rle_reader_t *p_reader, const uint8_t *p_data, size_t len,
              uint32_t bit_width);

/* The next value. False at the end of the buffer or on any fault. */
bool rle_next(rle_reader_t *p_reader, uint32_t *p_out);

/* True when every read so far was well formed. */
bool rle_ok(const rle_reader_t *p_reader);

/*
 * The number of bits that can hold every value from zero to this one.
 *
 * Zero when the largest value is zero.
 */
uint32_t rle_bit_width_for(uint32_t largest);

#endif /* RLE_H */
