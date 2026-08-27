/*
 * The run length and bit packed hybrid, tested in isolation.
 *
 * Every byte sequence here is written in full and checked by hand against the
 * specification. The bit order matters most in this file. A decoder written
 * from a drawing of a byte reverses every value. On a dictionary index, the
 * reversal gives a valid index to the wrong word, and a round trip does not
 * detect it.
 */
#include "check.h"
#include "rle.h"

/* Reads count values, and reports whether they are the expected ones. */
static bool reads(const uint8_t *p_bytes, size_t len, uint32_t bit_width,
                  const uint32_t *p_want, size_t count)
{
    rle_reader_t reader;
    size_t i = 0U;
    bool b_same = true;

    rle_open(&reader, p_bytes, len, bit_width);

    for (i = 0U; (i < count) && b_same; i++)
    {
        uint32_t got = 0U;

        b_same = rle_next(&reader, &got) && (got == p_want[i]);
    }

    return b_same && rle_ok(&reader);
}

static void a_width_of_zero_gives_zeroes_and_reads_nothing(void)
{
    static const uint32_t p_want[4] = { 0U, 0U, 0U, 0U };

    /*
     * In a column without null values, the largest definition level is zero.
     * The level then needs zero bits, and the page does not store any bytes
     * for it.
     */
    CHECK_TRUE(reads(NULL, 0U, 0U, p_want, 4U),
               "a width of zero gives as many zeroes as are asked for");
    CHECK_EQUAL(rle_bit_width_for(0U), 0U, "and the width for zero is zero");
}

static void a_bit_width_holds_every_value_up_to_the_largest(void)
{
    CHECK_EQUAL(rle_bit_width_for(1U), 1U, "one value above zero needs a bit");
    CHECK_EQUAL(rle_bit_width_for(2U), 2U, "two need two");
    CHECK_EQUAL(rle_bit_width_for(3U), 2U, "and three still need two");
    CHECK_EQUAL(rle_bit_width_for(4U), 3U, "four need three");
    CHECK_EQUAL(rle_bit_width_for(255U), 8U, "and 255 needs eight");
}

/*
 * A repeated run. The header is the count shifted left by one, with the low
 * bit clear. The value follows in as many whole bytes as the width needs.
 */
static void a_repeated_run_gives_one_value_many_times(void)
{
    /* Header 0x08 is 4 << 1, so four values. Width 1 needs one byte. */
    static const uint8_t p_bytes[2] = { 0x08U, 0x01U };
    static const uint32_t p_want[4] = { 1U, 1U, 1U, 1U };

    CHECK_TRUE(reads(p_bytes, 2U, 1U, p_want, 4U),
               "a repeated run gives its value the number of times it says");
}

static void a_repeated_run_takes_whole_bytes_for_its_value(void)
{
    /* Header 0x04 is 2 << 1, so two values. Width 9 needs two bytes. */
    static const uint8_t p_bytes[3] = { 0x04U, 0x2CU, 0x01U };
    static const uint32_t p_want[2] = { 300U, 300U };

    CHECK_TRUE(reads(p_bytes, 3U, 9U, p_want, 2U),
               "a width of nine takes two bytes, lowest byte first");
}

/*
 * A bit packed run fills each byte from its lowest bit upwards.
 *
 * The header is the group count shifted left by one, with the low bit set. A
 * group is eight values.
 */
static void a_bit_packed_run_fills_each_byte_from_the_low_bit(void)
{
    /*
     * Header 0x03 is (1 << 1) | 1, so one group of eight values. At one bit
     * each, the group fills one byte. 0xB1 is 1011 0001, and from the low bit
     * its values are 1, 0, 0, 0, 1, 1, 0, 1.
     */
    static const uint8_t p_bytes[2] = { 0x03U, 0xB1U };
    static const uint32_t p_want[8] = { 1U, 0U, 0U, 0U, 1U, 1U, 0U, 1U };

    CHECK_TRUE(reads(p_bytes, 2U, 1U, p_want, 8U),
               "the low bit of the byte is the first value");
}

/* A value wider than the remaining bits of a byte continues in the next. */
static void a_packed_value_carries_across_a_byte_boundary(void)
{
    /*
     * One group of eight values at three bits each is 24 bits, so three bytes.
     * The values 0 to 7 packed low bit first give 0x88, 0xC6, 0xFA.
     */
    static const uint8_t p_bytes[4] = { 0x03U, 0x88U, 0xC6U, 0xFAU };
    static const uint32_t p_want[8] = { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U };

    CHECK_TRUE(reads(p_bytes, 4U, 3U, p_want, 8U),
               "a three bit value spans a byte boundary without reversing");
}

/* The two run shapes follow one another in the same buffer. */
static void the_two_run_shapes_follow_one_another(void)
{
    static const uint8_t p_bytes[4] = {
        0x04U, 0x01U,   /* repeated: two values of one */
        0x03U, 0x0FU    /* packed: eight values, 0000 1111 read low first */
    };
    static const uint32_t p_want[10] = {
        1U, 1U,
        1U, 1U, 1U, 1U, 0U, 0U, 0U, 0U
    };

    CHECK_TRUE(reads(p_bytes, 4U, 1U, p_want, 10U),
               "a repeated run and a packed run read one after the other");
}

static void a_buffer_that_runs_out_is_refused(void)
{
    /* A packed run that claims a group, but the buffer ends at the header. */
    static const uint8_t p_bytes[1] = { 0x03U };
    rle_reader_t reader;
    uint32_t got = 0U;

    rle_open(&reader, p_bytes, 1U, 8U);
    CHECK_FALSE(rle_next(&reader, &got), "a run without its bytes is refused");
    CHECK_FALSE(rle_ok(&reader), "and the fault is recorded");
}

static void a_bit_width_wider_than_a_value_is_refused(void)
{
    rle_reader_t reader;

    rle_open(&reader, NULL, 0U, 33U);
    CHECK_FALSE(rle_ok(&reader),
                "a width past 32 bits cannot hold a dictionary index");
}

void suite_rle(void)
{
    check_suite("rle");

    a_width_of_zero_gives_zeroes_and_reads_nothing();
    a_bit_width_holds_every_value_up_to_the_largest();
    a_repeated_run_gives_one_value_many_times();
    a_repeated_run_takes_whole_bytes_for_its_value();
    a_bit_packed_run_fills_each_byte_from_the_low_bit();
    a_packed_value_carries_across_a_byte_boundary();
    the_two_run_shapes_follow_one_another();
    a_buffer_that_runs_out_is_refused();
    a_bit_width_wider_than_a_value_is_refused();

    return;
}
