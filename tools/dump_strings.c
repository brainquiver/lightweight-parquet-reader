/*
 * Prints a digest of one string column, so that it can be diffed against
 * pyarrow.
 *
 * This program belongs to the oracle harness, and the library does not
 * include it. The text itself would print as gigabytes, so each row group
 * gives a count and one digest over every value. The digest takes the length
 * and then the bytes of each value. The lengths in the digest make two
 * adjacent values with a wrong split give a different digest.
 *
 *     dump-strings <file.parquet> <column>
 */
#include "parquet.h"

#include <stdio.h>
#include <stdlib.h>

/* FNV-1a, 64 bit, which is the digest the comparison script rebuilds. */
static uint64_t fold(uint64_t hash, const uint8_t *p_bytes, size_t len)
{
    size_t i = 0U;

    for (i = 0U; i < len; i++)
    {
        hash ^= (uint64_t)p_bytes[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

/* The length, as eight bytes, so a wrong split is visible. */
static uint64_t fold_length(uint64_t hash, size_t len)
{
    uint8_t p_wide[8];
    size_t i = 0U;

    for (i = 0U; i < 8U; i++)
    {
        p_wide[i] = (uint8_t)((len >> (i * 8U)) & 0xFFU);
    }

    return fold(hash, p_wide, 8U);
}

int main(int argc, char **argv)
{
    parquet_file_t *p_file = NULL;
    parquet_string_t *p_values = NULL;
    FILE *p_in = NULL;
    uint8_t *p_bytes = NULL;
    long size = 0;
    size_t got = 0U;
    size_t group = 0U;
    size_t cap = 0U;
    int result = 0;

    if (3 != argc)
    {
        (void)fprintf(stderr, "usage: %s <file.parquet> <column>\n", argv[0]);
        return 2;
    }

    p_in = fopen(argv[1], "rb");

    if (NULL == p_in)
    {
        (void)fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    (void)fseek(p_in, 0, SEEK_END);
    size = ftell(p_in);
    (void)fseek(p_in, 0, SEEK_SET);
    /* CAST: a length from ftell is never negative after a good seek. */
    p_bytes = malloc((size_t)size);
    got = (NULL == p_bytes) ? 0U : fread(p_bytes, 1U, (size_t)size, p_in);
    (void)fclose(p_in);

    if (got != (size_t)size)
    {
        free(p_bytes);
        return 2;
    }

    p_file = parquet_open(p_bytes, got);

    if (NULL == p_file)
    {
        free(p_bytes);
        return 1;
    }

    for (group = 0U; group < parquet_row_groups(p_file); group++)
    {
        size_t want = parquet_row_group_rows(p_file, group);
        size_t count = 0U;
        uint64_t digest = 14695981039346656037ULL;
        size_t i = 0U;

        if (cap < want)
        {
            free(p_values);
            p_values = malloc(want * sizeof(parquet_string_t));
            cap = (NULL == p_values) ? 0U : want;
        }

        if (!parquet_read_strings(p_file, argv[2], group, p_values, cap,
                                  &count))
        {
            result = 1;
            break;
        }

        for (i = 0U; i < count; i++)
        {
            if (NULL == p_values[i].p_bytes)
            {
                digest = fold_length(digest, (size_t)-1);
            }
            else
            {
                digest = fold_length(digest, p_values[i].len);
                digest = fold(digest, p_values[i].p_bytes, p_values[i].len);
            }
        }

        printf("rg %zu values %zu digest %016llx\n", group, count,
               (unsigned long long)digest);
    }

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);

    return result;
}
