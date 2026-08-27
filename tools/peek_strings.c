/*
 * Prints the first values of a string column, so that a disagreement is
 * visible.
 *
 * The oracle harness prints digests, which show that two readers disagree.
 * This program shows where: it prints the head of each value, escaped, for the
 * first few rows of the first row group.
 *
 *     peek-strings <file.parquet> <column> <count>
 */
#include "parquet.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    parquet_file_t *p_file = NULL;
    parquet_string_t *p_values = NULL;
    FILE *p_in = NULL;
    uint8_t *p_bytes = NULL;
    long size = 0;
    size_t got = 0U;
    size_t want = 0U;
    size_t count = 0U;
    size_t i = 0U;
    int result = 0;

    if (4 != argc)
    {
        (void)fprintf(stderr, "usage: %s <file> <column> <count>\n", argv[0]);
        return 2;
    }

    want = (size_t)strtoul(argv[3], NULL, 10);
    p_in = fopen(argv[1], "rb");

    if (NULL == p_in)
    {
        return 2;
    }

    (void)fseek(p_in, 0, SEEK_END);
    size = ftell(p_in);
    (void)fseek(p_in, 0, SEEK_SET);
    /* CAST: a length from ftell is never negative after a good seek. */
    p_bytes = malloc((size_t)size);
    got = (NULL == p_bytes) ? 0U : fread(p_bytes, 1U, (size_t)size, p_in);
    (void)fclose(p_in);
    p_file = parquet_open(p_bytes, got);

    if (NULL == p_file)
    {
        free(p_bytes);
        return 1;
    }

    p_values = malloc(parquet_row_group_rows(p_file, 0U) *
                      sizeof(parquet_string_t));

    if (!parquet_read_strings(p_file, argv[2], 0U, p_values,
                              parquet_row_group_rows(p_file, 0U), &count))
    {
        /* A refused read must not look like an empty column. */
        result = 1;
    }
    else
    {
        if (0U == want)
        {
            /* Raw, length prefixed, so the bytes can be compared exactly. */
            for (i = 0U; i < count; i++)
            {
                uint8_t p_wide[8];
                size_t k = 0U;

                for (k = 0U; k < 8U; k++)
                {
                    p_wide[k] = (uint8_t)((p_values[i].len >> (k * 8U)) &
                                          0xFFU);
                }

                (void)fwrite(p_wide, 1U, 8U, stdout);
                (void)fwrite(p_values[i].p_bytes, 1U, p_values[i].len, stdout);
            }
        }

        for (i = 0U; (i < want) && (i < count); i++)
        {
            size_t show = (p_values[i].len < 60U) ? p_values[i].len : 60U;

            printf("%zu len %zu [", i, p_values[i].len);

            if (NULL != p_values[i].p_bytes)
            {
                size_t k = 0U;

                for (k = 0U; k < show; k++)
                {
                    uint8_t byte = p_values[i].p_bytes[k];

                    printf("%c", ((byte >= 0x20U) && (byte < 0x7FU))
                                 ? (char)byte : '.');
                }
            }

            printf("]\n");
        }
    }

    free(p_values);
    parquet_close(p_file);
    free(p_bytes);

    return result;
}
