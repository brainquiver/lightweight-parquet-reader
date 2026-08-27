"""Compares the C string reader against pyarrow, row group by row group.

The C tool prints one line for each row group: the value count, and a digest
over the length and then the bytes of every value. This script rebuilds the
same digest from pyarrow and diffs the lines. The lengths in the digest make
two adjacent values with a wrong split give a different digest.
"""
import subprocess
import sys

import pyarrow.parquet as pq

OFFSET = 14695981039346656037
PRIME = 1099511628211
MASK = (1 << 64) - 1


def fold(hash_value, data):
    for byte in data:
        hash_value = ((hash_value ^ byte) * PRIME) & MASK
    return hash_value


def fold_length(hash_value, length):
    return fold(hash_value, (length & MASK).to_bytes(8, "little"))


def expected(path, column):
    pf = pq.ParquetFile(path)
    out = []
    for group in range(pf.metadata.num_row_groups):
        table = pf.read_row_group(group, columns=[column])
        digest = OFFSET
        count = 0
        for value in table.column(column):
            raw = value.as_py()
            if raw is None:
                digest = fold_length(digest, -1)
            else:
                data = raw.encode("utf-8") if isinstance(raw, str) else raw
                digest = fold_length(digest, len(data))
                digest = fold(digest, data)
            count += 1
        out.append(f"rg {group} values {count} digest {digest:016x}")
    return out


def main():
    binary, column, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    bad = 0
    for path in files:
        got = subprocess.run([binary, path, column], capture_output=True,
                             text=True)
        if 0 != got.returncode:
            print(f"REFUSED {path}\n  {got.stderr.strip()}")
            bad += 1
            continue
        mine = got.stdout.splitlines()
        theirs = expected(path, column)
        if mine != theirs:
            bad += 1
            print(f"DIFFERS {path}")
            for line, (a, b) in enumerate(zip(mine, theirs)):
                if a != b:
                    print(f"  C       {a}\n  pyarrow {b}")
                    break
            else:
                print(f"  lengths: C {len(mine)}, pyarrow {len(theirs)}")
    print(f"{len(files)} files, {len(files) - bad} agree, {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
