/* rle.h describes the two run shapes and the bit order that they use. */
#include "rle.h"

/* A bit packed run counts groups, and each group holds this many values. */
#define GROUP_VALUES 8U

/* The bits in one byte, which the bit reader consumes lowest first. */
#define BYTE_BITS 8U

/* The widest decoded value. A dictionary index is at most 32 bits. */
#define BIT_WIDTH_MAX 32U

void rle_open(rle_reader_t *p_reader, const uint8_t *p_data, size_t len,
              uint32_t bit_width)
{
    p_reader->p_data = p_data;
    p_reader->len = len;
    p_reader->at = 0U;
    p_reader->bit_at = 0U;
    p_reader->bit_width = bit_width;
    p_reader->run_left = 0U;
    p_reader->b_packed = false;
    p_reader->repeated = 0U;
    p_reader->b_bad = (BIT_WIDTH_MAX < bit_width);

    return;
}

bool rle_ok(const rle_reader_t *p_reader)
{
    return !p_reader->b_bad;
}

uint32_t rle_bit_width_for(uint32_t largest)
{
    uint32_t bits = 0U;

    while (0U != largest)
    {
        bits++;
        largest >>= 1;
    }

    return bits;
}

/* An unsigned varint, as the run headers use. */
static uint64_t take_varint(rle_reader_t *p_reader)
{
    uint64_t out = 0U;
    uint32_t shift = 0U;
    bool b_more = true;

    while (b_more && !p_reader->b_bad)
    {
        if ((p_reader->at >= p_reader->len) || (63U < shift))
        {
            p_reader->b_bad = true;
        }
        else
        {
            uint8_t byte = p_reader->p_data[p_reader->at];

            p_reader->at++;
            out |= ((uint64_t)(byte & 0x7FU)) << shift;
            shift += 7U;
            b_more = (0U != (byte & 0x80U));
        }
    }

    return p_reader->b_bad ? 0U : out;
}

/*
 * Take this many bits, lowest bit of each byte first.
 *
 * At most eight bits are taken from one byte in a pass, so the mask below can
 * never shift by the width of its own type.
 */
static uint32_t take_bits(rle_reader_t *p_reader, uint32_t bits)
{
    uint32_t out = 0U;
    uint32_t taken = 0U;

    while ((taken < bits) && !p_reader->b_bad)
    {
        uint32_t left_in_byte = BYTE_BITS - p_reader->bit_at;
        uint32_t want = bits - taken;
        uint32_t take = (want < left_in_byte) ? want : left_in_byte;

        if (p_reader->at >= p_reader->len)
        {
            p_reader->b_bad = true;
        }
        else
        {
            uint32_t byte = (uint32_t)p_reader->p_data[p_reader->at];
            uint32_t mask = ((uint32_t)1U << take) - 1U;

            out |= ((byte >> p_reader->bit_at) & mask) << taken;
            taken += take;
            p_reader->bit_at += take;

            if (BYTE_BITS == p_reader->bit_at)
            {
                p_reader->bit_at = 0U;
                p_reader->at++;
            }
        }
    }

    return out;
}

/* The value a repeated run carries, in as many bytes as the width needs. */
static uint32_t take_repeated(rle_reader_t *p_reader)
{
    uint32_t bytes = (p_reader->bit_width + (BYTE_BITS - 1U)) / BYTE_BITS;
    uint32_t out = 0U;
    uint32_t i = 0U;

    for (i = 0U; (i < bytes) && !p_reader->b_bad; i++)
    {
        if (p_reader->at >= p_reader->len)
        {
            p_reader->b_bad = true;
        }
        else
        {
            out |= ((uint32_t)p_reader->p_data[p_reader->at]) << (i * 8U);
            p_reader->at++;
        }
    }

    return out;
}

/* Open the next run. False at the end of the buffer. */
static bool next_run(rle_reader_t *p_reader)
{
    uint64_t header = 0U;
    bool b_ok = (p_reader->at < p_reader->len);

    if (b_ok)
    {
        header = take_varint(p_reader);
        b_ok = !p_reader->b_bad;
    }

    if (b_ok)
    {
        p_reader->b_packed = (0U != (header & 1U));

        if (p_reader->b_packed)
        {
            /*
             * A packed run starts on a byte boundary, and its group count is
             * multiplied by eight to give the number of values that it holds.
             */
            p_reader->bit_at = 0U;
            p_reader->run_left = (header >> 1) * (uint64_t)GROUP_VALUES;
        }
        else
        {
            p_reader->run_left = header >> 1;
            p_reader->repeated = take_repeated(p_reader);
        }

        b_ok = !p_reader->b_bad && (0U != p_reader->run_left);
    }

    return b_ok;
}

bool rle_next(rle_reader_t *p_reader, uint32_t *p_out)
{
    bool b_ok = !p_reader->b_bad;

    *p_out = 0U;

    if (b_ok && (0U == p_reader->bit_width))
    {
        /*
         * A width of zero means that every value is zero, so the reader does
         * not read the buffer and returns a zero for every call.
         */
        b_ok = true;
    }
    else
    {
        if (b_ok && (0U == p_reader->run_left))
        {
            b_ok = next_run(p_reader);
        }

        if (b_ok)
        {
            *p_out = p_reader->b_packed
                     ? take_bits(p_reader, p_reader->bit_width)
                     : p_reader->repeated;
            p_reader->run_left--;
            b_ok = !p_reader->b_bad;
        }
    }

    return b_ok;
}
