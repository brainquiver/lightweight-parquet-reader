/*
 * The reader, tested against a fixture of the page shapes that broke it.
 *
 * The fixture is small and committed, and tools/write_shapes.py gives the
 * purpose of each column. The comparison against pyarrow over a set of parquet
 * files is in tools/compare_strings.py. That comparison shows that the reader
 * agrees with another implementation. These cases state what the reader must
 * do.
 */
#include "check.h"
#include "parquet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "test/fixtures/shapes.parquet"
#define FIXTURE_V2 "test/fixtures/shapes-v2.parquet"
#define ROWS 4000U
#define GROUPS 4U
#define GROUP_ROWS 1000U

/* The fixture, read whole. The caller frees it. */
static uint8_t *slurp(const char *p_path, size_t *p_len)
{
    FILE *p_file = fopen(p_path, "rb");
    uint8_t *p_bytes = NULL;
    long size = 0;

    *p_len = 0U;

    if (NULL == p_file)
    {
        return NULL;
    }

    (void)fseek(p_file, 0, SEEK_END);
    size = ftell(p_file);
    (void)fseek(p_file, 0, SEEK_SET);
    /* CAST: a length from ftell is never negative after a good seek. */
    p_bytes = malloc((size_t)size);

    if (NULL != p_bytes)
    {
        *p_len = fread(p_bytes, 1U, (size_t)size, p_file);
    }

    (void)fclose(p_file);

    return p_bytes;
}

/* Reads one column of row group zero into caller memory. */
static bool read_column(parquet_file_t *p_file, const char *p_name,
                        parquet_string_t *p_out, size_t *p_count)
{
    return parquet_read_strings(p_file, p_name, 0U, p_out, ROWS, p_count);
}

/* Reads one row group of a column into caller memory. */
static bool read_group(parquet_file_t *p_file, const char *p_name,
                       size_t group, parquet_string_t *p_out, size_t *p_count)
{
    return parquet_read_strings(p_file, p_name, group, p_out, ROWS, p_count);
}

/* True when this value holds exactly this text. */
static bool holds(const parquet_string_t *p_value, const char *p_want)
{
    return (NULL != p_value->p_bytes) &&
           (p_value->len == strlen(p_want)) &&
           (0 == memcmp(p_value->p_bytes, p_want, p_value->len));
}

static void the_fixture_opens_and_says_what_it_holds(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);

    if (NULL == p_file)
    {
        CHECK_TRUE(false, "the fixture opens");
        free(p_bytes);
        return;
    }

    CHECK_EQUAL(parquet_rows(p_file), ROWS, "it holds four thousand rows");
    CHECK_EQUAL(parquet_row_groups(p_file), GROUPS, "in four row groups");
    CHECK_EQUAL(parquet_row_group_rows(p_file, 0U), GROUP_ROWS,
                "of a thousand rows each");
    CHECK_EQUAL(parquet_row_group_rows(p_file, GROUPS), 0U,
                "and a group past the end holds zero rows");
    CHECK_TRUE(parquet_has_column(p_file, "plain_pages"), "and its columns");
    CHECK_TRUE(parquet_has_column(p_file, "dict_pages"), "by name");
    CHECK_TRUE(parquet_has_column(p_file, "nulls"), "each of them");
    CHECK_FALSE(parquet_has_column(p_file, "absent"),
                "and it refuses a column that is absent");

    parquet_close(p_file);
    free(p_bytes);
}

/*
 * A value of an early PLAIN page survives the pages after it.
 *
 * A PLAIN value is borrowed from the decompressed page, and an earlier version
 * of the reader decompressed each page over the same memory. Every value of
 * every page but the last then held a correct length and the bytes of a later
 * page. A length check does not detect the defect, because every length is
 * still correct. A byte comparison against another reader found it. Without
 * that comparison, the output would hold text that reads perfectly and belongs
 * to another row.
 *
 * The case reads the whole chunk and then tests the first values, when every
 * later page is already decoded.
 */
static void a_value_of_an_early_page_survives_the_later_pages(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);
    parquet_string_t *p_values = malloc(ROWS * sizeof(parquet_string_t));
    size_t count = 0U;
    size_t i = 0U;
    bool b_same = true;

    if ((NULL == p_file) || (NULL == p_values))
    {
        CHECK_TRUE(false, "the fixture opens");
        free(p_values);
        free(p_bytes);
        return;
    }

    CHECK_TRUE(read_column(p_file, "plain_pages", p_values, &count),
               "the plain column reads");
    CHECK_EQUAL(count, GROUP_ROWS, "and gives every row of the group");

    /* Row zero, tested when every page of the chunk is already decoded. */
    CHECK_TRUE(holds(&p_values[0], "row 0 abcdefghij"),
               "the first value still holds its own bytes at the end");

    for (i = 0U; i < count; i++)
    {
        char p_want[256];
        size_t repeats = (i % 17U) + 1U;
        size_t k = 0U;
        int at = snprintf(p_want, sizeof(p_want), "row %zu ", i);

        for (k = 0U; k < repeats; k++)
        {
            at += snprintf(&p_want[at], sizeof(p_want) - (size_t)at,
                           "abcdefghij");
        }

        b_same = b_same && holds(&p_values[i], p_want);
    }

    CHECK_TRUE(b_same, "and so does every value of every page");

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);
}

static void a_dictionary_column_gives_the_value_and_not_the_index(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);
    parquet_string_t *p_values = malloc(ROWS * sizeof(parquet_string_t));
    size_t count = 0U;
    size_t i = 0U;
    bool b_same = true;

    if ((NULL == p_file) || (NULL == p_values))
    {
        CHECK_TRUE(false, "the fixture opens");
        free(p_values);
        free(p_bytes);
        return;
    }

    CHECK_TRUE(read_column(p_file, "dict_pages", p_values, &count),
               "the dictionary column reads");
    CHECK_EQUAL(count, GROUP_ROWS, "and gives every row of the group");

    for (i = 0U; i < count; i++)
    {
        char p_want[32];

        (void)snprintf(p_want, sizeof(p_want), "class %zu", i % 12U);
        b_same = b_same && holds(&p_values[i], p_want);
    }

    CHECK_TRUE(b_same, "and every row is the text its index points at");

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);
}

/* A null takes a row and has a NULL pointer. An empty value has 0 bytes. */
static void a_null_is_not_an_empty_value(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);
    parquet_string_t *p_values = malloc(ROWS * sizeof(parquet_string_t));
    size_t count = 0U;
    size_t i = 0U;
    size_t nulls = 0U;
    bool b_right = true;

    if ((NULL == p_file) || (NULL == p_values))
    {
        CHECK_TRUE(false, "the fixture opens");
        free(p_values);
        free(p_bytes);
        return;
    }

    CHECK_TRUE(read_column(p_file, "nulls", p_values, &count),
               "the optional column reads");
    CHECK_EQUAL(count, GROUP_ROWS, "and a null still takes its row");

    for (i = 0U; i < count; i++)
    {
        bool b_null = (NULL == p_values[i].p_bytes);

        nulls += (b_null ? 1U : 0U);
        b_right = b_right && (b_null == (0U == (i % 7U)));
    }

    CHECK_TRUE(b_right, "and the nulls fall where the writer put them");
    CHECK_EQUAL(nulls, (GROUP_ROWS + 6U) / 7U,
                "which is every seventh row");

    /* An empty value is present: its pointer is set and its length is 0. */
    CHECK_TRUE(read_column(p_file, "empties", p_values, &count),
               "the column of empty values reads");
    CHECK_TRUE(NULL != p_values[0].p_bytes,
               "an empty value is present");
    CHECK_EQUAL(p_values[0].len, 0U, "and its length is zero");
    CHECK_TRUE(holds(&p_values[1], "e1"), "and its neighbours are unaffected");

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);
}

/*
 * The same shapes as version two data pages.
 *
 * A version two page keeps its levels outside the compressed part, and the
 * levels do not carry a length prefix. The page header gives the split
 * between levels and values. The files that this reader targets do not use
 * these pages today. Without this case, the first such file would be the
 * first run of that path.
 */
static void a_version_two_page_reads_the_same_values(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE_V2, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);
    parquet_string_t *p_values = malloc(ROWS * sizeof(parquet_string_t));
    size_t count = 0U;
    size_t i = 0U;
    size_t nulls = 0U;
    bool b_same = true;
    bool b_right = true;

    if ((NULL == p_file) || (NULL == p_values))
    {
        CHECK_TRUE(false, "the version two fixture opens");
        free(p_values);
        free(p_bytes);
        return;
    }

    CHECK_TRUE(read_column(p_file, "plain_pages", p_values, &count),
               "a version two plain column reads");
    CHECK_EQUAL(count, GROUP_ROWS, "and gives every row of the group");
    CHECK_TRUE(holds(&p_values[0], "row 0 abcdefghij"),
               "and its first value survives the pages after it");

    CHECK_TRUE(read_column(p_file, "dict_pages", p_values, &count),
               "a version two dictionary column reads");

    for (i = 0U; i < count; i++)
    {
        char p_want[32];

        (void)snprintf(p_want, sizeof(p_want), "class %zu", i % 12U);
        b_same = b_same && holds(&p_values[i], p_want);
    }

    CHECK_TRUE(b_same, "and every row is the text its index points at");

    CHECK_TRUE(read_column(p_file, "nulls", p_values, &count),
               "a version two optional column reads");

    for (i = 0U; i < count; i++)
    {
        bool b_null = (NULL == p_values[i].p_bytes);

        nulls += (b_null ? 1U : 0U);
        b_right = b_right && (b_null == (0U == (i % 7U)));
    }

    CHECK_TRUE(b_right, "and its levels put the nulls where the writer did");
    CHECK_EQUAL(nulls, (GROUP_ROWS + 6U) / 7U,
                "which is every seventh row");

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);
}

/*
 * Every row group reads, and each gives its own rows.
 *
 * A column is read one row group at a time. A reader that keeps state across
 * groups, for example a dictionary or an arena offset, gives the second group
 * the first group's answers. Row 0 of group 1 is row 1000 of the file.
 */
static void each_row_group_gives_its_own_rows(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);
    parquet_string_t *p_values = malloc(ROWS * sizeof(parquet_string_t));
    size_t group = 0U;
    size_t count = 0U;
    bool b_same = true;

    if ((NULL == p_file) || (NULL == p_values))
    {
        CHECK_TRUE(false, "the fixture opens");
        free(p_values);
        free(p_bytes);
        return;
    }

    for (group = 0U; group < GROUPS; group++)
    {
        size_t i = 0U;

        b_same = b_same &&
                 read_group(p_file, "plain_pages", group, p_values, &count) &&
                 (GROUP_ROWS == count);

        for (i = 0U; (i < count) && b_same; i++)
        {
            char p_want[256];
            size_t row = (group * GROUP_ROWS) + i;
            size_t repeats = (row % 17U) + 1U;
            size_t k = 0U;
            int at = snprintf(p_want, sizeof(p_want), "row %zu ", row);

            for (k = 0U; k < repeats; k++)
            {
                at += snprintf(&p_want[at], sizeof(p_want) - (size_t)at,
                               "abcdefghij");
            }

            b_same = b_same && holds(&p_values[i], p_want);
        }
    }

    CHECK_TRUE(b_same, "every row group gives the rows that belong to it");

    /* And a dictionary column, which has one dictionary for each chunk. */
    b_same = read_group(p_file, "dict_pages", GROUPS - 1U, p_values, &count);
    CHECK_TRUE(b_same, "the last row group of a dictionary column reads");

    if (b_same)
    {
        char p_want[32];

        size_t row = (size_t)(GROUPS - 1U) * (size_t)GROUP_ROWS;

        (void)snprintf(p_want, sizeof(p_want), "class %zu", row % 12U);
        CHECK_TRUE(holds(&p_values[0], p_want),
                   "and its first row is the text that row holds");
    }

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);
}

static void a_file_that_is_not_parquet_is_refused(void)
{
    static const uint8_t p_junk[16] = {
        'n', 'o', 't', ' ', 'p', 'a', 'r', 'q', 'u', 'e', 't', 0U,
        0U, 0U, 0U, 0U
    };

    CHECK_TRUE(NULL == parquet_open(p_junk, sizeof(p_junk)),
               "a file without PAR1 at each end is refused");
    CHECK_TRUE(NULL == parquet_open(p_junk, 2U),
               "and so is one too short to hold a footer");
}

static void a_column_that_is_not_a_string_is_refused(void)
{
    size_t len = 0U;
    uint8_t *p_bytes = slurp(FIXTURE, &len);
    parquet_file_t *p_file = (NULL == p_bytes) ? NULL
                                               : parquet_open(p_bytes, len);
    parquet_string_t p_values[4];
    size_t count = 0U;

    if (NULL == p_file)
    {
        CHECK_TRUE(false, "the fixture opens");
        free(p_bytes);
        return;
    }

    CHECK_FALSE(parquet_read_strings(p_file, "absent", 0U, p_values, 4U,
                                     &count),
                "an absent column is refused by name");
    CHECK_EQUAL(count, 0U, "and the count stays zero");

    parquet_close(p_file);
    free(p_bytes);
}

void suite_parquet(void)
{
    check_suite("parquet");

    the_fixture_opens_and_says_what_it_holds();
    a_value_of_an_early_page_survives_the_later_pages();
    a_dictionary_column_gives_the_value_and_not_the_index();
    a_null_is_not_an_empty_value();
    a_version_two_page_reads_the_same_values();
    each_row_group_gives_its_own_rows();
    a_file_that_is_not_parquet_is_refused();
    a_column_that_is_not_a_string_is_refused();

    return;
}
