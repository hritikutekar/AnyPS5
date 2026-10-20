#!/usr/bin/env python3
# bda-table-lookup.py <bda-table.bin> <address> [<address> ...]
# Replays the shader's probe_bda_pointer (SpirvBdaLookup.cpp DefineBdaLookup, indexed or plain table)
# on a BDA page table read from GPU memory (umr-watch-hang.sh bda-table.bin), for 4-byte reads at
# each address, and checks the whole table for entries whose device address is outside 0xffff8...
import struct
import sys

data = open(sys.argv[1], "rb").read()
words = struct.unpack_from("<%dI" % (len(data) // 4), data)
version, count, entry_bytes, reserved = words[0:4]
print("table bytes 0x%x: version %d count %d entryBytes %d reserved %d" % (len(data), version, count, entry_bytes, reserved))


def u64(at):
    return words[at] | (words[at + 1] << 32)


ranges = [struct.unpack_from("<QQQII", data, 16 + 32 * i) for i in range(count)]
bad = [(i, r) for i, r in enumerate(ranges) if (r[2] >> 47) != 0x1FFFF]
unsorted = sum(1 for i in range(1, count) if ranges[i][0] < ranges[i - 1][0])
print("entries with a device address outside 0xffff8...: %d; out-of-order begins: %d" % (len(bad), unsorted))
for i, r in bad[:20]:
    print("  [%d] 0x%x-0x%x -> 0x%x perm %d res %d" % (i, r[0], r[1], r[2], r[3], r[4]))

length = len(words)
available = length - 4
count_fits = count <= available >> 3
range_words = (count if count_fits else 0) * 8
index_words = available - range_words
indexed = reserved == 1
header_fits = indexed and count_fits and index_words >= 5
index_at = 4 + range_words if header_fits else 0
index_base = u64(index_at)
buckets = words[index_at + 2]
shift = words[index_at + 3]
print("indexed %s headerFits %s indexBase 0x%x buckets %d shift %d" % (indexed, header_fits, index_base, buckets, shift))


def probe(address, nbytes=4):
    end = address + nbytes
    below = address < index_base
    bucket = ((address - index_base) & (2**64 - 1)) >> shift
    beyond = bucket >= buckets
    in_bucket = indexed and not (below or beyond)
    start_at = (index_at + 4 + bucket) if in_bucket else 0
    outside = 0 if below else count
    start_high = (words[start_at + 1] if in_bucket else outside) if indexed else count
    search_high = count if count < start_high else start_high
    start_low = (words[start_at] if in_bucket else outside) if indexed else 0
    search_low = search_high if search_high < start_low else start_low
    lo, hi = search_low, search_high
    while lo < hi:
        mid = lo + ((hi - lo) >> 1)
        if address < u64(4 + mid * 8):
            hi = mid
        else:
            lo = mid + 1
    if lo == 0:
        return "unmapped (index 0)", None
    sel = 4 + (lo - 1) * 8
    begin, finish, base, perm, res = u64(sel), u64(sel + 2), u64(sel + 4), words[sel + 6], words[sel + 7]
    result = (base + address - begin) & (2**64 - 1)
    why = None
    if res != 0 or begin >= finish: why = "invalid table"
    elif end > finish: why = "unmapped"
    elif perm & 1 == 0: why = "permission"
    elif base == 0: why = "invalid table"
    elif result < base: why = "overflow"
    text = "search [%d,%d) -> entry %d 0x%x-0x%x -> 0x%x" % (search_low, search_high, lo - 1, begin, finish, base)
    # the same lookup over the whole table, for comparison
    lo2, hi2 = 0, count
    while lo2 < hi2:
        mid = lo2 + ((hi2 - lo2) >> 1)
        if address < u64(4 + mid * 8): hi2 = mid
        else: lo2 = mid + 1
    if lo2 != lo: text += "  (FULL SEARCH GIVES ENTRY %d)" % (lo2 - 1)
    return text + ("  FAIL " + why if why else ""), None if why else result


for arg in sys.argv[2:]:
    address = int(arg, 0)
    text, result = probe(address)
    print("0x%x: %s%s" % (address, text, "  => 0x%x" % result if result is not None else ""))
