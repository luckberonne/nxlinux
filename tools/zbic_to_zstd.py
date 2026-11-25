#!/usr/bin/env python3
"""ZBIC → standard Zstandard. ZBIC (system 22.0.0+, NSO flag bit 7) is zstd with two changes: the frame magic is 'ZBIC', and every FSE table description (the
normalized counts of the literal-length, offset and match-length tables of a sequences section, and of the table that codes Huffman weights) is written with binary
interpolative coding instead of zstd's variable-bit counts. Everything else, block and section headers and all bitstreams, is zstd as specified in RFC 8878.

to_zstd() rewrites a ZBIC frame as a standard frame by re-coding those descriptions and fixing the sizes around them, without decoding the data; decompress() then hands
the result to a zstd decoder (Python 3.14's compression.zstd, or the zstandard package). What the coding is was read from github.com/kinnay/zbic (its FSE_readNCount_bic);
this is an independent implementation, and the order table is generated here rather than copied.

  zbic_to_zstd.py IN.zbic OUT          (decompresses one frame)"""
import sys


class Error(ValueError):
    pass


def _order():
    """The order the cumulative counts are decoded in: (middle, first, last) of [0, 256] halved, the upper half before the lower, then one closing entry."""
    rows = []
    def rec(lo, hi):
        if hi - lo < 2: return
        mid = (lo + hi) // 2
        rows.append((mid, lo, hi)); rec(mid, hi); rec(lo, mid)
    rec(0, 256)
    return rows + [(0, 0, 1)]

ORDER = _order()


def read_ncount_bic(d, at, end):
    """(normalized counts, table log, bytes used) of the description at d[at:end]. A count of -1 is zstd's 'less than one' probability."""
    if at >= end: raise Error('FSE table description is missing')
    left, low = d[at] & 0x7F, d[at] >> 7            # payload bytes, read from the last one back; whether 'less than one' counts are used
    used = left + 1
    if used >= end - at: raise Error('FSE table description runs past its section')
    def fill(x):                                    # the state takes bytes while it is below 2^40
        nonlocal left
        while left and not x >> 40:
            x = x << 8 | d[at + left]; left -= 1
        return x
    x = fill(0)
    nsym, x = x % 52 + 1, fill(x // 52)             # the highest symbol
    log, x = (x & 7) + 5, fill(x >> 3)
    size = 1 << log
    last, x = x % size + 1, fill(x // size)
    if low: last += nsym + 1
    p = 1 << nsym.bit_length()                      # the power of two above the highest symbol
    if p > 0xFF: raise Error('FSE table description has too many symbols')
    c = [0] * 257
    c[p] = last
    for mid, lo, hi in ORDER[256 - p:]:
        if c[lo] == c[hi]:
            for i in range(lo + 1, hi): c[i] = c[lo]
        else:
            span = c[hi] - c[lo] + 1
            c[mid] = c[lo] + x % span
            x = fill(x // span)
    counts, acc, remaining = [], 0, size
    for s in range(nsym + 1):
        n = c[s + 1] - acc - low
        acc = c[s + 1]
        if n < -1: raise Error('FSE table description has a negative count')
        counts.append(n); remaining -= abs(n)
    if remaining or left: raise Error('FSE table description does not add up')
    return counts, log, used


def write_ncount(counts, log):
    """zstd's description of normalized counts (RFC 8878 section 4.1.1)."""
    while counts and counts[-1] == 0: counts = counts[:-1]
    bits, n = log - 5, 4
    remaining, threshold, nb = (1 << log) + 1, 1 << log, log + 1
    s, prev0 = 0, False
    while s < len(counts) and remaining > 1:
        if prev0:
            start = s
            while s < len(counts) and counts[s] == 0: s += 1
            if s == len(counts): break
            while s >= start + 24: start += 24; bits |= 0xFFFF << n; n += 16
            while s >= start + 3: start += 3; bits |= 3 << n; n += 2
            bits |= (s - start) << n; n += 2
        count = counts[s]; s += 1
        mx = 2 * threshold - 1 - remaining
        remaining -= abs(count)
        count += 1
        if count >= threshold: count += mx
        bits |= count << n
        n += nb - (count < mx)
        prev0 = count == 1
        if remaining < 1: raise Error('FSE counts exceed the table')
        while remaining < threshold: nb -= 1; threshold >>= 1
    if remaining != 1 or s != len(counts): raise Error('FSE counts do not fill the table')
    return bits.to_bytes((n + 7) // 8, 'little')


def _recode(d, at, end):
    """The standard description of the ZBIC one at d[at:end], and the bytes the ZBIC one took."""
    counts, log, used = read_ncount_bic(d, at, end)
    return write_ncount(counts, log), used


def _block(d, at, end):
    """A compressed block's content d[at:end] with its FSE table descriptions re-coded."""
    if end - at < 1: raise Error('empty compressed block')
    b0 = d[at]
    kind, fmt = b0 & 3, b0 >> 2 & 3
    if kind < 2:                                    # raw or RLE literals: nothing to re-code
        hsz = (1, 2, 1, 3)[fmt]
        regen = int.from_bytes(d[at:at + hsz], 'little') >> (3 if hsz == 1 else 4)
        lit_end = at + hsz + (regen if kind == 0 else 1)
        lit = d[at:lit_end]
    else:
        hsz, nbits = ((3, 10), (3, 10), (4, 14), (5, 18))[fmt]
        v = int.from_bytes(d[at:at + hsz], 'little') >> 4
        regen, comp = v & (1 << nbits) - 1, v >> nbits
        body, lit_end = at + hsz, at + hsz + comp
        if lit_end > end: raise Error('literals run past the block')
        data = d[body:lit_end]
        if kind == 2 and comp and d[body] < 128:    # Huffman weights coded with FSE: one table description at the start
            tree_end = body + 1 + d[body]
            if tree_end > lit_end: raise Error('Huffman tree runs past the literals')
            new, used = _recode(d, body + 1, tree_end)
            tree = new + d[body + 1 + used:tree_end]
            if len(tree) >= 128: raise Error('Huffman tree description grows past 127 bytes in standard coding')
            data = bytes([len(tree)]) + tree + d[tree_end:lit_end]
        comp = len(data)
        if fmt == 0:
            if comp > 1023: raise Error('single-stream literals grow past 1023 bytes in standard coding')
        else:
            fmt = max(fmt, 1 if comp < 1 << 10 else 2 if comp < 1 << 14 else 3)
            if comp >= 1 << 18: raise Error('literals too large')
        hsz, nbits = ((3, 10), (3, 10), (4, 14), (5, 18))[fmt]
        lit = (kind | fmt << 2 | regen << 4 | comp << 4 + nbits).to_bytes(hsz, 'little') + data
    if lit_end > end: raise Error('literals run past the block')
    # sequences section: the count, the modes byte, then up to three table descriptions (literal lengths, offsets, match lengths)
    p = lit_end
    if p >= end: raise Error('sequences section is missing')
    n0 = d[p]
    p += 1 if n0 < 128 else 2 if n0 < 255 else 3
    if n0 == 0: return lit + d[lit_end:end]
    if p >= end: raise Error('sequences section is cut short')
    modes = d[p]; p += 1
    out = [lit, d[lit_end:p]]
    for shift in (6, 4, 2):
        mode = modes >> shift & 3
        if mode == 1:
            out.append(d[p:p + 1]); p += 1
        elif mode == 2:
            new, used = _recode(d, p, end)
            out.append(new); p += used
    if p > end: raise Error('sequence tables run past the block')
    out.append(d[p:end])
    return b''.join(out)


def to_zstd(d):
    """The standard zstd frame of the ZBIC frame d."""
    d = bytes(d)
    if d[:4] != b'ZBIC': raise Error(f'not a ZBIC frame (starts {d[:4].hex()})')
    if len(d) < 6: raise Error('ZBIC frame is cut short')
    fhd = d[4]
    p = 5 + (0 if fhd & 0x20 else 1) + (0, 1, 2, 4)[fhd & 3] + ((1 if fhd & 0x20 else 0), 2, 4, 8)[fhd >> 6]
    out = [b'\x28\xb5\x2f\xfd', d[4:p]]
    while True:
        if p + 3 > len(d): raise Error('ZBIC frame ends inside a block header')
        bh = int.from_bytes(d[p:p + 3], 'little')
        last, kind, size = bh & 1, bh >> 1 & 3, bh >> 3
        p += 3
        if kind == 3: raise Error('reserved block type')
        stored = 1 if kind == 1 else size
        if p + stored > len(d): raise Error('ZBIC frame ends inside a block')
        if kind == 2:
            body = _block(d, p, p + size)
            if len(body) > 1 << 17: raise Error('block grows past 128 KiB in standard coding')
            out += [(last | 2 << 1 | len(body) << 3).to_bytes(3, 'little'), body]
        else:
            out.append(d[p - 3:p + stored])
        p += stored
        if last: break
    out.append(d[p:p + (4 if fhd & 4 else 0)])
    if p + (4 if fhd & 4 else 0) != len(d): raise Error('bytes after the end of the ZBIC frame')
    return b''.join(out)


def decompress(d):
    """The data of the ZBIC frame d."""
    std = to_zstd(d)
    try:
        from compression import zstd
        try: return zstd.decompress(std)
        except zstd.ZstdError as e: raise Error(f'the re-coded frame does not decompress ({e})') from None
    except ImportError:
        pass
    try: import zstandard
    except ImportError: raise Error('decompressing needs Python 3.14 (compression.zstd) or the zstandard package') from None
    try: return zstandard.ZstdDecompressor().decompressobj().decompress(std)
    except zstandard.ZstdError as e: raise Error(f'the re-coded frame does not decompress ({e})') from None


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    try: data = decompress(open(sys.argv[1], 'rb').read())
    except Error as e: sys.exit(f'[RED] {sys.argv[1]}: {e}')
    open(sys.argv[2], 'wb').write(data)
    print(f'{len(data):#x} bytes')
