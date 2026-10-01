#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Minimal LZX cabinet writer used to generate test data for the LZX decoder.

It produces real LZX bitstreams (verbatim, aligned-offset and uncompressed
blocks, repeated-offset matches, pretree run-length codes, blocks spanning
32 KiB frames) so the decoder can be exercised without Windows tooling.
Output can be cross-checked with `cabextract`.

usage: make_lzx_cab.py OUT.cab --window 16 --mode verbatim|aligned|uncompressed|mixed \
           [--block-size N] FILE[=NAME] ...
"""
import argparse
import heapq
import struct

FRAME = 32768
MIN_MATCH, MAX_MATCH = 2, 257
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}
EXTRA = [0 if i < 4 else ((i - 2) // 2 if i < 36 else 17) for i in range(51)]
BASE = [0]
for i in range(50):
    BASE.append(BASE[-1] + (1 << EXTRA[i]))


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, value, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 16:
                self.out += struct.pack("<H", self.acc)
                self.acc = 0
                self.n = 0

    def align16(self):
        if self.n:
            self.write(0, 16 - self.n)

    def raw(self, data):
        assert self.n == 0
        self.out += data


def huffman_lengths(freqs, max_len=16):
    freqs = list(freqs)
    while True:
        items = [(f, i) for i, f in enumerate(freqs) if f > 0]
        lens = [0] * len(freqs)
        if not items:
            return lens
        if len(items) == 1:
            # a lone code would be incomplete; pair it with a dummy symbol
            lens[items[0][1]] = 1
            lens[1 if items[0][1] == 0 else 0] = 1
            return lens
        heap = [(f, k, [i]) for k, (f, i) in enumerate(items)]
        heapq.heapify(heap)
        k = len(heap)
        while len(heap) > 1:
            f1, _, a = heapq.heappop(heap)
            f2, _, b = heapq.heappop(heap)
            for s in a + b:
                lens[s] += 1
            heapq.heappush(heap, (f1 + f2, k, a + b))
            k += 1
        if max(lens) <= max_len:
            return lens
        freqs = [(f + 1) // 2 if f else 0 for f in freqs]


def canonical_codes(lens):
    codes = [0] * len(lens)
    code = 0
    for length in range(1, 17):
        for s, l in enumerate(lens):
            if l == length:
                codes[s] = code
                code += 1
        code <<= 1
    return codes


class Tree:
    def __init__(self, lens):
        self.lens = lens
        self.codes = canonical_codes(lens)

    def put(self, bw, sym):
        assert self.lens[sym] > 0, sym
        bw.write(self.codes[sym], self.lens[sym])


def encode_lengths(bw, new, prev):
    """Write a run of code lengths with the pretree, using all four code kinds."""
    syms = []  # (pretree symbol, extra bits value, extra bit count)
    i = 0
    n = len(new)
    while i < n:
        if new[i] == 0:
            j = i
            while j < n and new[j] == 0 and j - i < 51:
                j += 1
            run = j - i
            if run >= 20:
                syms.append((18, run - 20, 5))
                i = j
                continue
            if run >= 4:
                syms.append((17, run - 4, 4))
                i = j
                continue
        j = i
        while j < n and new[j] == new[i] and j - i < 5:
            j += 1
        if j - i >= 4 and new[i] != 0:
            run = j - i
            z = (prev[i] - new[i]) % 17
            syms.append((19, run - 4, 1))
            syms.append((z, 0, 0))
            # code 19: the delta is taken against prev[i] and the result repeated
            i = j
            continue
        z = (prev[i] - new[i]) % 17
        syms.append((z, 0, 0))
        i += 1
    freq = [0] * 20
    for s, _, _ in syms:
        freq[s] += 1
    plens = huffman_lengths(freq, 15)
    for l in plens:
        bw.write(l, 4)
    tree = Tree(plens)
    for s, v, b in syms:
        tree.put(bw, s)
        if b:
            bw.write(v, b)


def lz77(data, start, end, window, frame_end_of):
    """Greedy matcher; matches never cross a 32 KiB frame boundary."""
    tokens = []
    head = {}
    i = start
    while i < end:
        best_len, best_off = 0, 0
        limit = min(end, frame_end_of(i)) - i
        if limit >= 3:
            key = data[i:i + 3]
            for cand in reversed(head.get(key, [])[-16:]):
                off = i - cand
                if off > window - 3:
                    continue
                l = 0
                while l < min(limit, MAX_MATCH) and data[cand + l] == data[i + l]:
                    l += 1
                if l > best_len:
                    best_len, best_off = l, off
        if best_len >= 3:
            tokens.append(("m", best_len, best_off))
            for k in range(best_len):
                if i + k + 3 <= len(data):
                    head.setdefault(data[i + k:i + k + 3], []).append(i + k)
            i += best_len
        else:
            tokens.append(("l", data[i]))
            if i + 3 <= len(data):
                head.setdefault(data[i:i + 3], []).append(i)
            i += 1
    return tokens


def compress(data, window_bits, mode, block_size):
    window = 1 << window_bits
    slots = SLOTS[window_bits]
    nmain = 256 + slots * 8
    bw = BitWriter()
    bw.write(0, 1)  # no E8 translation
    prev_main = [0] * nmain
    prev_len = [0] * 249
    R = [1, 1, 1]
    frame_offsets = []  # compressed byte offset at each frame end
    pos = 0
    block_no = 0
    total = len(data)

    def frame_end_of(p):
        return (p // FRAME + 1) * FRAME

    next_frame = FRAME
    while pos < total:
        bsize = min(block_size, total - pos)
        kind = mode
        if mode == "mixed":
            kind = ["verbatim", "aligned", "uncompressed"][block_no % 3]
        block_no += 1
        if kind == "uncompressed":
            bw.write(3, 3)
            bw.write(bsize >> 8, 16)
            bw.write(bsize & 0xFF, 8)
            # decoder discards 1..16 bits to reach a word boundary
            if bw.n == 0:
                bw.write(0, 16)
            else:
                bw.align16()
            bw.raw(struct.pack("<III", *R))
            chunk_start = pos
            while pos < chunk_start + bsize:
                take = min(chunk_start + bsize, next_frame) - pos
                bw.raw(data[pos:pos + take])
                pos += take
                if pos == next_frame or pos == total:
                    frame_offsets.append(len(bw.out))
                    next_frame += FRAME
            if bsize & 1:
                bw.raw(b"\0")
                frame_offsets[-1] = len(bw.out) if pos == total else frame_offsets[-1]
            continue

        tokens = lz77(data, pos, pos + bsize, window, frame_end_of)
        # Resolve offsets into (main element, length symbol, footer info) with R0-R2
        enc = []
        r = list(R)
        main_f = [0] * nmain
        len_f = [0] * 249
        al_f = [0] * 8
        for t in tokens:
            if t[0] == "l":
                enc.append(("l", t[1]))
                main_f[t[1]] += 1
                continue
            _, length, off = t
            if off == r[0]:
                slot, footer = 0, None
            elif off == r[1]:
                slot, footer = 1, None
                r[0], r[1] = r[1], r[0]
            elif off == r[2]:
                slot, footer = 2, None
                r[0], r[2] = r[2], r[0]
            else:
                fo = off + 2
                slot = max(s for s in range(slots) if BASE[s] <= fo)
                footer = fo - BASE[slot]
                r[2], r[1], r[0] = r[1], r[0], off
            lh = min(length - 2, 7)
            me = 256 + slot * 8 + lh
            main_f[me] += 1
            ls = length - 2 - 7 if lh == 7 else None
            if ls is not None:
                len_f[ls] += 1
            if footer is not None and kind == "aligned" and EXTRA[slot] >= 3:
                al_f[footer & 7] += 1
            enc.append(("m", me, ls, slot, footer, length))
        R = r
        main_lens = huffman_lengths(main_f)
        len_lens = huffman_lengths(len_f)
        bw.write(2 if kind == "aligned" else 1, 3)
        bw.write(bsize >> 8, 16)
        bw.write(bsize & 0xFF, 8)
        if kind == "aligned":
            al_lens = huffman_lengths(al_f, 7)
            if sum(1 for x in al_lens if x) < 2:  # keep the aligned tree usable
                al_lens = [3] * 8
            for l in al_lens:
                bw.write(l, 3)
            al_tree = Tree(al_lens)
        encode_lengths(bw, main_lens[:256], prev_main[:256])
        encode_lengths(bw, main_lens[256:], prev_main[256:])
        encode_lengths(bw, len_lens, prev_len)
        prev_main, prev_len = main_lens, len_lens
        main_tree, len_tree = Tree(main_lens), Tree(len_lens)
        for e in enc:
            if e[0] == "l":
                main_tree.put(bw, e[1])
                pos += 1
            else:
                _, me, ls, slot, footer, length = e
                main_tree.put(bw, me)
                if ls is not None:
                    len_tree.put(bw, ls)
                if footer is not None:
                    extra = EXTRA[slot]
                    if kind == "aligned" and extra >= 3:
                        bw.write(footer >> 3, extra - 3)
                        al_tree.put(bw, footer & 7)
                    elif extra:
                        bw.write(footer, extra)
                pos += length
            if pos == next_frame or pos == total:
                bw.align16()
                frame_offsets.append(len(bw.out))
                next_frame += FRAME
    bw.align16()
    if not frame_offsets or frame_offsets[-1] != len(bw.out):
        frame_offsets[-1:] = [len(bw.out)]
    return bytes(bw.out), frame_offsets


def write_cab(path, files, window_bits, mode, block_size):
    folder_data = b"".join(d for _, d in files)
    comp, offsets = compress(folder_data, window_bits, mode, block_size)
    nframes = (len(folder_data) + FRAME - 1) // FRAME
    assert len(offsets) == nframes, (len(offsets), nframes)
    blocks = []
    start = 0
    for f in range(nframes):
        end = offsets[f]
        ucb = min(FRAME, len(folder_data) - f * FRAME)
        blocks.append((comp[start:end], ucb))
        start = end
    cffiles = b""
    off = 0
    for name, d in files:
        cffiles += struct.pack("<IIHHHH", len(d), off, 0, 0x5821, 0x0000, 0x20) + name.encode() + b"\0"
        off += len(d)
    header_size = 36
    folder_size = 8
    files_off = header_size + folder_size
    data_off = files_off + len(cffiles)
    cfdata = b"".join(struct.pack("<IHH", 0, len(c), u) + c for c, u in blocks)
    total = data_off + len(cfdata)
    hdr = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, total, 0, files_off, 0, 3, 1, 1, len(files), 0, 0x1234, 0)
    folder = struct.pack("<IHH", data_off, len(blocks), 3 | (window_bits << 8))
    with open(path, "wb") as fh:
        fh.write(hdr + folder + cffiles + cfdata)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--window", type=int, default=16)
    ap.add_argument("--mode", default="verbatim")
    ap.add_argument("--block-size", type=int, default=50000)
    a = ap.parse_args()
    files = []
    for spec in a.files:
        src, _, name = spec.partition("=")
        files.append((name or src.split("/")[-1], open(src, "rb").read()))
    write_cab(a.out, files, a.window, a.mode, a.block_size)


if __name__ == "__main__":
    main()
