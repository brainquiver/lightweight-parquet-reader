/*
 * Prints the footer of a parquet file, so that it can be diffed against
 * pyarrow.
 *
 * This program belongs to the oracle harness, and the library does not
 * include it. It prints one line for the file, one for each field of the
 * schema and one for each column chunk, in a shape that a script can compare.
 *
 *     dump-meta <file.parquet>
 */
#include "meta.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    meta_file_t meta;
    FILE *p_file = NULL;
    uint8_t *p_bytes = NULL;
    long size = 0;
    size_t got = 0U;
    size_t i = 0U;
    int result = 0;

    if (2 != argc)
    {
        (void)fprintf(stderr, "usage: %s <file.parquet>\n", argv[0]);
        return 2;
    }

    p_file = fopen(argv[1], "rb");

    if (NULL == p_file)
    {
        (void)fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    (void)fseek(p_file, 0, SEEK_END);
    size = ftell(p_file);
    (void)fseek(p_file, 0, SEEK_SET);
    /* CAST: a file length from ftell is never negative after a good seek. */
    p_bytes = malloc((size_t)size);
    got = (NULL == p_bytes) ? 0U : fread(p_bytes, 1U, (size_t)size, p_file);
    (void)fclose(p_file);

    if (got != (size_t)size)
    {
        (void)fprintf(stderr, "short read of %s\n", argv[1]);
        free(p_bytes);
        return 2;
    }

    if (!meta_read(p_bytes, got, &meta))
    {
        free(p_bytes);
        return 1;
    }

    printf("file %lld rows %zu fields %zu row_groups\n",
           (long long)meta.num_rows, meta.fields, meta.row_groups);

    for (i = 0U; i < meta.fields; i++)
    {
        printf("field %zu %s type %d\n", i, meta.p_field[i].p_name,
               meta.p_field[i].type);
    }

    for (i = 0U; i < meta.row_groups; i++)
    {
        size_t k = 0U;

        for (k = 0U; k < meta.fields; k++)
        {
            const meta_chunk_t *p_chunk = meta_chunk_of(&meta, i, k);

            printf("chunk %zu %zu values %lld codec %s data_at %lld "
                   "dict_at %lld size %lld enc %u\n",
                   i, k, (long long)p_chunk->num_values,
                   meta_codec_name(p_chunk->codec),
                   (long long)p_chunk->data_page_offset,
                   (long long)p_chunk->dictionary_page_offset,
                   (long long)p_chunk->total_compressed_size,
                   p_chunk->encodings);
        }
    }

    meta_free(&meta);
    free(p_bytes);

    return result;
}
