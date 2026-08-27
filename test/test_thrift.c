/*
 * The Thrift compact protocol reader, tested in isolation.
 *
 * Every byte sequence here is written in full, so it can be checked by hand
 * against the protocol. This matters more than usual, because this layer does
 * not have an oracle of its own. Every field of a parquet file is read through
 * it, so a fault here reaches every layer and looks like a fault elsewhere.
 */
#include "check.h"
#include "thrift.h"

#include <string.h>

/* A reader over a byte sequence that the case writes in full. */
static void open_over(thrift_reader_t *p_reader, const uint8_t *p_bytes,
                      size_t len)
{
    thrift_open(p_reader, p_bytes, len);

    return;
}

static void a_varint_holds_seven_bits_in_each_byte(void)
{
    static const uint8_t p_one[1] = { 0x01U };
    static const uint8_t p_hundred[1] = { 0x64U };
    static const uint8_t p_low[2] = { 0x80U, 0x01U };
    static const uint8_t p_max_byte[2] = { 0xFFU, 0x01U };
    static const uint8_t p_big[3] = { 0x80U, 0x80U, 0x01U };
    thrift_reader_t reader;

    open_over(&reader, p_one, 1U);
    CHECK_EQUAL(thrift_varint(&reader), 1U, "one byte holds a small value");

    open_over(&reader, p_hundred, 1U);
    CHECK_EQUAL(thrift_varint(&reader), 100U, "and every value below 128");

    open_over(&reader, p_low, 2U);
    CHECK_EQUAL(thrift_varint(&reader), 128U,
                "128 needs a second byte, and the low seven bits come first");

    open_over(&reader, p_max_byte, 2U);
    CHECK_EQUAL(thrift_varint(&reader), 255U, "255 is 0xFF then 0x01");

    open_over(&reader, p_big, 3U);
    CHECK_EQUAL(thrift_varint(&reader), 16384U, "and 2 to the 14th is three");
}

static void a_varint_that_never_ends_is_refused(void)
{
    static const uint8_t p_forever[12] = {
        0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U,
        0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U
    };
    thrift_reader_t reader;

    open_over(&reader, p_forever, 12U);
    (void)thrift_varint(&reader);
    CHECK_FALSE(thrift_ok(&reader),
                "a varint past ten bytes cannot hold 64 bits, so it refuses");
}

static void a_varint_past_the_end_is_refused(void)
{
    static const uint8_t p_cut[1] = { 0x80U };
    thrift_reader_t reader;

    open_over(&reader, p_cut, 1U);
    CHECK_EQUAL(thrift_varint(&reader), 0U, "a cut varint returns zero");
    CHECK_FALSE(thrift_ok(&reader), "and the fault is recorded");
}

/*
 * The zigzag order puts the sign in the low bit, so a small negative value
 * takes as few bytes as a small positive value.
 */
static void zigzag_alternates_around_zero(void)
{
    static const uint8_t p_case[6] = { 0x00U, 0x01U, 0x02U, 0x03U, 0x04U,
                                       0x05U };
    static const int64_t p_want[6] = { 0, -1, 1, -2, 2, -3 };
    thrift_reader_t reader;
    size_t i = 0U;
    bool b_same = true;

    for (i = 0U; i < 6U; i++)
    {
        open_over(&reader, &p_case[i], 1U);
        b_same = b_same && (thrift_zigzag(&reader) == p_want[i]);
    }

    CHECK_TRUE(b_same, "zero, then minus one, then one, and so on outwards");
}

/*
 * A field header carries the difference from the field before it. A
 * difference of zero means that the identifier follows as a zigzag varint.
 */
static void a_field_header_carries_a_delta(void)
{
    static const uint8_t p_bytes[4] = {
        0x15U,          /* delta 1, type 5 (I32), so field 1 */
        0x25U,          /* delta 2, type 5, so field 3 */
        0x05U, 0x28U,   /* delta 0, type 5, then field 20 as zigzag */
        };
    thrift_reader_t reader;
    uint8_t type = 0U;
    int16_t id = 0;

    open_over(&reader, p_bytes, 4U);

    CHECK_TRUE(thrift_field(&reader, &type, &id), "the first field reads");
    CHECK_EQUAL(id, 1U, "a delta of one from zero is field one");
    CHECK_EQUAL(type, THRIFT_I32, "and the low nibble is the type");

    CHECK_TRUE(thrift_field(&reader, &type, &id), "the second field reads");
    CHECK_EQUAL(id, 3U, "a delta of two from one is field three");

    CHECK_TRUE(thrift_field(&reader, &type, &id), "the third field reads");
    CHECK_EQUAL(id, 20U,
                "and a delta of zero takes the identifier that follows");
}

static void a_stop_byte_ends_a_structure(void)
{
    static const uint8_t p_bytes[2] = { 0x15U, 0x00U };
    thrift_reader_t reader;
    uint8_t type = 0U;
    int16_t id = 0;

    open_over(&reader, p_bytes, 2U);
    CHECK_TRUE(thrift_field(&reader, &type, &id), "the field reads");
    CHECK_FALSE(thrift_field(&reader, &type, &id), "and the stop byte ends it");
    CHECK_TRUE(thrift_ok(&reader), "which is a clean end");
}

/*
 * The delta restarts inside a nested structure.
 *
 * The reader must keep a stack of last identifiers. With a single last
 * identifier, the fields of the outer structure take the identifiers of the
 * inner one. Every identifier is still valid, so the reader does not refuse,
 * and the document silently changes its meaning.
 */
static void the_delta_restarts_inside_a_nested_structure(void)
{
    static const uint8_t p_bytes[6] = {
        0x3CU,          /* delta 3, type 12 (STRUCT), so outer field 3 */
        0x15U,          /* inside it: delta 1 from zero, so inner field 1 */
        0x00U,          /* stop, which ends the inner structure */
        0x25U,          /* delta 2 from three, so outer field 5 */
        0x00U,          /* stop, which ends the outer structure */
        0x00U
    };
    thrift_reader_t reader;
    uint8_t type = 0U;
    int16_t id = 0;

    open_over(&reader, p_bytes, 6U);
    thrift_struct_begin(&reader);

    CHECK_TRUE(thrift_field(&reader, &type, &id), "the outer field reads");
    CHECK_EQUAL(id, 3U, "it is field three");
    CHECK_EQUAL(type, THRIFT_STRUCT, "and it holds a structure");

    thrift_struct_begin(&reader);
    CHECK_TRUE(thrift_field(&reader, &type, &id), "the inner field reads");
    CHECK_EQUAL(id, 1U, "and its delta counts from zero");
    CHECK_FALSE(thrift_field(&reader, &type, &id), "the inner structure ends");
    thrift_struct_end(&reader);

    CHECK_TRUE(thrift_field(&reader, &type, &id), "the next outer field reads");
    CHECK_EQUAL(id, 5U, "and its delta counts from three again");

    thrift_struct_end(&reader);
    CHECK_TRUE(thrift_ok(&reader), "and the walk held throughout");
}

static void a_list_header_gives_the_count_and_the_element_type(void)
{
    static const uint8_t p_short[1] = { 0x35U };  /* 3 elements of type 5 */
    /*
     * A count of fifteen in the nibble means that a varint follows. The twenty
     * values follow it in full, because the reader refuses a count that the
     * remaining bytes cannot supply.
     */
    static const uint8_t p_long[22] = {
        0xF5U, 0x14U,
        0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U,
        0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U
    };
    thrift_reader_t reader;
    uint8_t type = 0U;
    uint32_t count = 0U;

    open_over(&reader, p_short, 1U);
    CHECK_TRUE(thrift_list(&reader, &type, &count), "a short list reads");
    CHECK_EQUAL(count, 3U, "the high nibble is the count");
    CHECK_EQUAL(type, THRIFT_I32, "and the low nibble the element type");

    open_over(&reader, p_long, 22U);
    CHECK_TRUE(thrift_list(&reader, &type, &count), "a long list reads");
    CHECK_EQUAL(count, 20U, "a count of fifteen means a varint follows");

    open_over(&reader, p_long, 2U);
    CHECK_FALSE(thrift_list(&reader, &type, &count),
                "and a count the remaining bytes cannot supply is refused");
}

static void a_binary_field_is_borrowed_and_never_copied(void)
{
    static const uint8_t p_bytes[5] = { 0x04U, 'a', 'b', 'c', 'd' };
    thrift_reader_t reader;
    const uint8_t *p_out = NULL;
    size_t len = 0U;

    open_over(&reader, p_bytes, 5U);
    CHECK_TRUE(thrift_binary(&reader, &p_out, &len), "the run reads");
    CHECK_EQUAL(len, 4U, "and its length is the prefix");
    CHECK_EQUAL_BYTES(p_out, len, (const uint8_t *)"abcd", 4U,
                      "and the bytes are the bytes");
    CHECK_TRUE(p_out == &p_bytes[1],
               "and they point into the buffer");
}

static void a_binary_field_longer_than_the_buffer_is_refused(void)
{
    static const uint8_t p_bytes[3] = { 0x40U, 'a', 'b' };
    thrift_reader_t reader;
    const uint8_t *p_out = NULL;
    size_t len = 0U;

    open_over(&reader, p_bytes, 3U);
    CHECK_FALSE(thrift_binary(&reader, &p_out, &len),
                "a run that claims 64 bytes of a 3 byte buffer is refused");
    CHECK_TRUE(NULL == p_out, "and the output stays NULL");
}

/* Every type must be skippable, or an unwanted field stops the walk. */
static void a_field_of_any_type_can_be_stepped_over(void)
{
    static const uint8_t p_bytes[14] = {
        0x18U,                          /* field 1, BINARY */
        0x03U, 'x', 'y', 'z',           /* three bytes */
        0x16U,                          /* field 2, I64 */
        0x08U,                          /* zigzag 4 */
        0x39U,                          /* field 3, LIST */
        0x26U,                          /* 2 elements of I64 */
        0x02U, 0x04U,                   /* the two values */
        0x1BU,                          /* field 4, MAP */
        0x00U,                          /* empty, so the type byte is absent */
        0x00U                           /* stop */
    };
    thrift_reader_t reader;
    uint8_t type = 0U;
    int16_t id = 0;
    size_t fields = 0U;

    open_over(&reader, p_bytes, 14U);

    while (thrift_field(&reader, &type, &id))
    {
        thrift_skip(&reader, type);
        fields++;
    }

    CHECK_EQUAL(fields, 4U, "every field was skipped");
    CHECK_TRUE(thrift_ok(&reader), "and the walk reached the stop byte");
}

void suite_thrift(void)
{
    check_suite("thrift");

    a_varint_holds_seven_bits_in_each_byte();
    a_varint_that_never_ends_is_refused();
    a_varint_past_the_end_is_refused();
    zigzag_alternates_around_zero();
    a_field_header_carries_a_delta();
    a_stop_byte_ends_a_structure();
    the_delta_restarts_inside_a_nested_structure();
    a_list_header_gives_the_count_and_the_element_type();
    a_binary_field_is_borrowed_and_never_copied();
    a_binary_field_longer_than_the_buffer_is_refused();
    a_field_of_any_type_can_be_stepped_over();

    return;
}
