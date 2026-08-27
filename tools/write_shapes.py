"""Writes the two fixture files that the parquet suite reads.

The shapes here are the ones that broke a reader.

    plain_pages   many PLAIN data pages in one chunk. A value of a PLAIN page
                  is borrowed from the decompressed page, so a reader that
                  decompresses the next page over the same memory returns the
                  correct length and the wrong bytes. Every length still
                  matches, so a length check does not detect the defect.
    dict_pages    a dictionary page and RLE_DICTIONARY data pages.
    nulls         an optional column, so the definition levels are read.
    empties       zero length values, which a reader must keep distinct from
                  nulls.

Each file holds four row groups. A column is read one row group at a time, so a
reader that keeps any state across groups reads the second group wrong.

A second file holds the same shapes as version two data pages. In these pages
the levels sit outside the compressed part, and the levels do not carry a
length prefix. The files that this reader targets do not use these pages
today. Without this file, the first such file would be the first run of that
path.

The script runs again only when those shapes change. The fixture files are
small and the suite reads them, so they are committed.
"""
import pathlib

import pyarrow as pa
import pyarrow.parquet as pq

HERE = pathlib.Path(__file__).resolve().parent.parent
OUT = HERE / "test" / "fixtures" / "shapes.parquet"
OUT_V2 = HERE / "test" / "fixtures" / "shapes-v2.parquet"

ROWS = 4000


def main():
    plain = [f"row {i} " + ("abcdefghij" * ((i % 17) + 1)) for i in range(ROWS)]
    # A small set of distinct values, so the writer keeps a dictionary.
    dictionary = [f"class {i % 12}" for i in range(ROWS)]
    nulls = [None if 0 == (i % 7) else f"maybe {i}" for i in range(ROWS)]
    empties = ["" if 0 == (i % 5) else f"e{i}" for i in range(ROWS)]

    table = pa.table({
        "plain_pages": pa.array(plain, pa.string()),
        "dict_pages": pa.array(dictionary, pa.string()),
        "nulls": pa.array(nulls, pa.string()),
        "empties": pa.array(empties, pa.string()),
    })
    pq.write_table(
        table, OUT,
        compression="zstd",
        # Small pages, so one chunk holds several, and a value of an early page
        # must survive the pages after it.
        data_page_size=4096,
        use_dictionary=["dict_pages"],
        write_statistics=False,
        version="1.0",
        row_group_size=1000,
    )
    pq.write_table(
        table, OUT_V2,
        compression="zstd",
        data_page_size=4096,
        data_page_version="2.0",
        use_dictionary=["dict_pages"],
        write_statistics=False,
        version="2.6",
        row_group_size=1000,
    )

    for path in (OUT, OUT_V2):
        pf = pq.ParquetFile(path)
        print(f"wrote {path.name} ({path.stat().st_size} bytes)")
        for c in range(pf.metadata.num_columns):
            col = pf.metadata.row_group(0).column(c)
            print(f"  {col.path_in_schema}: {col.compression} {col.encodings}")


if __name__ == "__main__":
    main()
