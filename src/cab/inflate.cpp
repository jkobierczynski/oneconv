// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Raw DEFLATE decoder (RFC 1951), used for MSZIP-compressed cabinets.
#include <array>
#include <cstring>

#include "cab.hpp"

namespace oneconv::cab {

namespace {

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint32_t bits(int count) {
        while (have_ < count) {
            uint32_t byte = pos_ < n_ ? p_[pos_] : 0;
            if (pos_ >= n_) {
                if (++overrun_ > 8) throw ParseError("deflate stream truncated");
            }
            ++pos_;
            buf_ |= byte << have_;
            have_ += 8;
        }
        uint32_t v = buf_ & ((1u << count) - 1);
        buf_ >>= count;
        have_ -= count;
        return v;
    }
    uint32_t peek(int count) {
        while (have_ < count) {
            uint32_t byte = pos_ < n_ ? p_[pos_] : 0;
            ++pos_;
            buf_ |= byte << have_;
            have_ += 8;
        }
        return buf_ & ((1u << count) - 1);
    }
    void drop(int count) {
        buf_ >>= count;
        have_ -= count;
    }
    void align() {
        buf_ = 0;
        have_ = 0;
    }
    size_t byte_pos() const { return pos_ - static_cast<size_t>(have_ / 8); }
    void set_byte_pos(size_t p) {
        pos_ = p;
        buf_ = 0;
        have_ = 0;
    }
    const uint8_t* data() const { return p_; }
    size_t size() const { return n_; }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    uint32_t buf_ = 0;
    int have_ = 0;
    int overrun_ = 0;
};

/// Huffman table indexed by the next 15 bits (LSB-first). Entry: symbol << 4 | length.
struct Huffman {
    std::vector<uint32_t> table;
    int max_len = 0;

    void build(const uint8_t* lens, int n) {
        int count[16] = {0};
        for (int i = 0; i < n; ++i) count[lens[i]]++;
        count[0] = 0;
        max_len = 0;
        for (int l = 1; l < 16; ++l)
            if (count[l]) max_len = l;
        int code = 0, next[16] = {0};
        for (int l = 1; l < 16; ++l) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        int bits = max_len == 0 ? 1 : max_len;
        table.assign(static_cast<size_t>(1) << bits, 0);
        for (int s = 0; s < n; ++s) {
            int len = lens[s];
            if (!len) continue;
            int c = next[len]++;
            // reverse the code bits
            int rev = 0;
            for (int k = 0; k < len; ++k) rev |= ((c >> k) & 1) << (len - 1 - k);
            for (int k = rev; k < (1 << bits); k += (1 << len))
                table[static_cast<size_t>(k)] = (static_cast<uint32_t>(s) << 4) | static_cast<uint32_t>(len);
        }
    }
    int decode(BitReader& br) const {
        int bits = max_len == 0 ? 1 : max_len;
        uint32_t e = table[br.peek(bits)];
        int len = static_cast<int>(e & 15);
        if (len == 0) throw ParseError("invalid deflate Huffman code");
        br.drop(len);
        return static_cast<int>(e >> 4);
    }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflate_codes(BitReader& br, const Huffman& lit, const Huffman& dist, Buffer& out, size_t max_out) {
    while (true) {
        int sym = lit.decode(br);
        if (sym < 256) {
            if (out.size() >= max_out) throw ParseError("deflate output overflow");
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw ParseError("invalid deflate length code");
            size_t len = kLenBase[sym] + br.bits(kLenExtra[sym]);
            int ds = dist.decode(br);
            if (ds >= 30) throw ParseError("invalid deflate distance code");
            size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (d > out.size()) throw ParseError("deflate distance too far back");
            if (out.size() + len > max_out) throw ParseError("deflate output overflow");
            size_t from = out.size() - d;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

}  // namespace

void inflate_append(const uint8_t* in, size_t size, Buffer& out, size_t max_out) {
    BitReader br(in, size);
    bool last = false;
    while (!last) {
        last = br.bits(1);
        int type = static_cast<int>(br.bits(2));
        if (type == 0) {
            size_t pos = br.byte_pos();
            br.set_byte_pos(pos);
            if (pos + 4 > size) throw ParseError("truncated stored deflate block");
            uint16_t len = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
            uint16_t nlen = static_cast<uint16_t>(in[pos + 2] | (in[pos + 3] << 8));
            if (static_cast<uint16_t>(~nlen) != len) throw ParseError("bad stored block length");
            pos += 4;
            if (pos + len > size || out.size() + len > max_out) throw ParseError("stored block out of range");
            out.insert(out.end(), in + pos, in + pos + len);
            br.set_byte_pos(pos + len);
        } else if (type == 1) {
            static Huffman fixed_lit, fixed_dist;
            static bool init = false;
            if (!init) {
                uint8_t l[288];
                for (int i = 0; i < 144; ++i) l[i] = 8;
                for (int i = 144; i < 256; ++i) l[i] = 9;
                for (int i = 256; i < 280; ++i) l[i] = 7;
                for (int i = 280; i < 288; ++i) l[i] = 8;
                fixed_lit.build(l, 288);
                uint8_t d[30];
                for (auto& x : d) x = 5;
                fixed_dist.build(d, 30);
                init = true;
            }
            inflate_codes(br, fixed_lit, fixed_dist, out, max_out);
        } else if (type == 2) {
            int hlit = static_cast<int>(br.bits(5)) + 257;
            int hdist = static_cast<int>(br.bits(5)) + 1;
            int hclen = static_cast<int>(br.bits(4)) + 4;
            static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {0};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = static_cast<uint8_t>(br.bits(3));
            Huffman clh;
            clh.build(cl, 19);
            uint8_t lens[320] = {0};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = clh.decode(br);
                if (sym < 16) {
                    lens[n++] = static_cast<uint8_t>(sym);
                } else {
                    int rep = 0;
                    uint8_t val = 0;
                    if (sym == 16) {
                        if (n == 0) throw ParseError("deflate repeat without previous length");
                        val = lens[n - 1];
                        rep = 3 + static_cast<int>(br.bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(br.bits(3));
                    } else {
                        rep = 11 + static_cast<int>(br.bits(7));
                    }
                    if (n + rep > hlit + hdist) throw ParseError("deflate code lengths overflow");
                    while (rep--) lens[n++] = val;
                }
            }
            Huffman lit, dist;
            lit.build(lens, hlit);
            dist.build(lens + hlit, hdist);
            inflate_codes(br, lit, dist, out, max_out);
        } else {
            throw ParseError("invalid deflate block type");
        }
    }
}

}  // namespace oneconv::cab
