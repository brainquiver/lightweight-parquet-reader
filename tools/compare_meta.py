"""Compares the C footer reader against pyarrow, file by file.

The C tool prints one line for the file, one for each field and one for each
column chunk. This script rebuilds the same lines from pyarrow and diffs them.
A disagreement prints the first line that differs, because on a file with five
hundred chunks, "the files differ" does not locate the fault.
"""
import subprocess
import sys

import pyarrow.parquet as pq

CODEC = {
    "UNCOMPRESSED": "UNCOMPRESSED", "SNAPPY": "SNAPPY", "GZIP": "GZIP",
    "LZO": "LZO", "BROTLI": "BROTLI", "LZ4": "LZ4", "ZSTD": "ZSTD",
    "LZ4_RAW": "LZ4_RAW",
}
ENCODING_BIT = {
    "PLAIN": 0, "PLAIN_DICTIONARY": 2, "RLE": 3, "BIT_PACKED": 4,
    "DELTA_BINARY_PACKED": 5, "DELTA_LENGTH_BYTE_ARRAY": 6,
    "DELTA_BYTE_ARRAY": 7, "RLE_DICTIONARY": 8, "BYTE_STREAM_SPLIT": 9,
}
TYPE = {
    "BOOLEAN": 0, "INT32": 1, "INT64": 2, "INT96": 3, "FLOAT": 4,
    "DOUBLE": 5, "BYTE_ARRAY": 6, "FIXED_LEN_BYTE_ARRAY": 7,
}


def expected(path):
    pf = pq.ParquetFile(path)
    md = pf.metadata
    out = [f"file {md.num_rows} rows {md.num_columns} fields "
           f"{md.num_row_groups} row_groups"]
    schema = md.schema
    for i in range(md.num_columns):
        column = schema.column(i)
        out.append(f"field {i} {column.name} type "
                   f"{TYPE[column.physical_type]}")
    for g in range(md.num_row_groups):
        for c in range(md.num_columns):
            col = md.row_group(g).column(c)
            bits = 0
            for name in col.encodings:
                bits |= 1 << ENCODING_BIT[name]
            dictionary = col.dictionary_page_offset or 0
            out.append(
                f"chunk {g} {c} values {col.num_values} codec "
                f"{CODEC[col.compression]} data_at {col.data_page_offset} "
                f"dict_at {dictionary} size {col.total_compressed_size} "
                f"enc {bits}")
    return out


def main():
    binary, files = sys.argv[1], sys.argv[2:]
    bad = 0
    for path in files:
        got = subprocess.run([binary, path], capture_output=True, text=True)
        if 0 != got.returncode:
            print(f"REFUSED {path}\n  {got.stderr.strip()}")
            bad += 1
            continue
        mine = got.stdout.splitlines()
        theirs = expected(path)
        if mine != theirs:
            bad += 1
            print(f"DIFFERS {path}")
            for line, (a, b) in enumerate(zip(mine, theirs)):
                if a != b:
                    print(f"  line {line + 1}\n    C       {a}\n"
                          f"    pyarrow {b}")
                    break
            else:
                print(f"  lengths: C {len(mine)}, pyarrow {len(theirs)}")
    print(f"{len(files)} files, {len(files) - bad} agree, {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
