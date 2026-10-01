// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// LZX decoder for Microsoft Cabinet folders ([MS-PATCH] LZX DELTA without the
// delta extensions; the format used by CAB files, including OneNote .onepkg).
#include <array>
#include <cstring>

#include "cab.hpp"

namespace oneconv::cab {

namespace {

constexpr int kNumChars = 256;
constexpr int kPretreeSymbols = 20;
constexpr int kLengthSymbols = 249;
constexpr int kAlignedSymbols = 8;
constexpr int kMaxMainSymbols = 256 + 50 * 8;
constexpr int kFrameSize = 32768;

enum BlockType { BlockInvalid = 0, BlockVerbatim = 1, BlockAligned = 2, BlockUncompressed = 3 };

/// Concatenated input blocks read as a byte stream.
class Input {
public:
    explicit Input(const std::vector<std::pair<const uint8_t*, size_t>>& blocks) {
        size_t total = 0;
        for (const auto& b : blocks) total += b.second;
        data_.reserve(total + 32);
        for (const auto& b : blocks) data_.insert(data_.end(), b.first, b.first + b.second);
    }
    size_t pos = 0;
    uint8_t byte() {
        if (pos >= data_.size()) {
            ++pos;
            if (pos > data_.size() + 64) throw ParseError("LZX input exhausted");
            return 0;
        }
        return data_[pos++];
    }
    size_t size() const { return data_.size(); }

private:
    Buffer data_;
};

/// MSB-first bit reader over 16-bit little-endian words.
class Bits {
public:
    explicit Bits(Input& in) : in_(in) {}

    void ensure(int n) {
        while (left_ < n) {
            uint32_t lo = in_.byte();
            uint32_t hi = in_.byte();
            uint64_t word = lo | (hi << 8);
            buf_ |= word << (48 - left_);
            left_ += 16;
        }
    }
    uint32_t peek(int n) {
        ensure(n);
        return static_cast<uint32_t>(buf_ >> (64 - n));
    }
    void remove(int n) {
        buf_ <<= n;
        left_ -= n;
    }
    uint32_t read(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        remove(n);
        return v;
    }
    int left() const { return left_; }
    /// Discard buffered bits and return to byte-level reading.
    void reset_to_bytes() {
        in_.pos -= static_cast<size_t>(left_ / 8);
        buf_ = 0;
        left_ = 0;
    }

private:
    Input& in_;
    uint64_t buf_ = 0;
    int left_ = 0;
};

/// Canonical Huffman decoder with a full 16-bit lookup table (MSB-first codes).
struct Tree {
    std::vector<uint16_t> sym;
    std::vector<uint8_t> len;
    bool empty = true;

    void build(const uint8_t* lens, int n) {
        int count[17] = {0};
        for (int i = 0; i < n; ++i) {
            if (lens[i] > 16) throw ParseError("LZX code length too long");
            count[lens[i]]++;
        }
        count[0] = 0;
        empty = true;
        for (int l = 1; l <= 16; ++l)
            if (count[l]) empty = false;
        sym.assign(1 << 16, 0);
        len.assign(1 << 16, 0);
        if (empty) return;
        int next[17] = {0};
        int code = 0;
        for (int l = 1; l <= 16; ++l) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        // Over-subscribed code check
        long long kraft = 0;
        for (int l = 1; l <= 16; ++l) kraft += static_cast<long long>(count[l]) << (16 - l);
        if (kraft > (1 << 16)) throw ParseError("LZX Huffman table over-subscribed");
        for (int s = 0; s < n; ++s) {
            int l = lens[s];
            if (!l) continue;
            int c = next[l]++;
            int start = c << (16 - l);
            int span = 1 << (16 - l);
            for (int k = 0; k < span; ++k) {
                sym[static_cast<size_t>(start + k)] = static_cast<uint16_t>(s);
                len[static_cast<size_t>(start + k)] = static_cast<uint8_t>(l);
            }
        }
    }
    int decode(Bits& b) const {
        if (empty) throw ParseError("LZX: decoding with an empty Huffman tree");
        uint32_t v = b.peek(16);
        int l = len[v];
        if (l == 0) throw ParseError("LZX: invalid Huffman code");
        b.remove(l);
        return sym[v];
    }
};

class Decoder {
public:
    Decoder(Input& in, int window_bits, size_t out_size) : in_(in), bits_(in), out_size_(out_size) {
        if (window_bits < 15 || window_bits > 21) throw ParseError("unsupported LZX window size");
        static const int slots[] = {30, 32, 34, 36, 38, 42, 50};
        position_slots_ = slots[window_bits - 15];
        window_size_ = static_cast<size_t>(1) << window_bits;
        main_symbols_ = kNumChars + position_slots_ * 8;
        for (size_t i = 0; i < 51; ++i)
            extra_bits_[i] = static_cast<uint8_t>(i < 4 ? 0 : (i < 36 ? (i - 2) / 2 : 17));
        position_base_[0] = 0;
        for (int i = 0; i < 50; ++i) position_base_[i + 1] = position_base_[i] + (1u << extra_bits_[i]);
        main_len_.fill(0);
        length_len_.fill(0);
    }

    Buffer run() {
        Buffer out;
        out.reserve(out_size_);
        window_.reserve(out_size_);
        size_t frame = 0;
        while (window_.size() < out_size_) {
            size_t frame_start = frame * kFrameSize;
            size_t frame_end = std::min(out_size_, frame_start + kFrameSize);
            if (!header_read_) {
                if (bits_.read(1)) {
                    uint32_t hi = bits_.read(16);
                    uint32_t lo = bits_.read(16);
                    intel_filesize_ = static_cast<int32_t>((hi << 16) | lo);
                }
                header_read_ = true;
            }
            decode_until(frame_end);
            // Realign the bitstream to 16 bits at each frame boundary
            if (block_type_ != BlockUncompressed && bits_.left() > 0) bits_.remove(bits_.left() & 15);
            size_t take = std::min(frame_end, window_.size()) - frame_start;
            size_t base = out.size();
            out.insert(out.end(), window_.begin() + static_cast<long>(frame_start),
                       window_.begin() + static_cast<long>(frame_start + take));
            if (intel_filesize_ != 0 && frame < 32768 && take > 10) e8_translate(out.data() + base, take, frame_start);
            ++frame;
        }
        out.resize(out_size_);
        return out;
    }

private:
    Input& in_;
    Bits bits_;
    size_t out_size_;
    size_t window_size_ = 0;
    int position_slots_ = 0;
    int main_symbols_ = 0;
    std::array<uint8_t, 51> extra_bits_{};
    std::array<uint32_t, 52> position_base_{};
    std::array<uint8_t, kMaxMainSymbols> main_len_{};
    std::array<uint8_t, kLengthSymbols> length_len_{};
    Tree main_, length_, aligned_;
    Buffer window_;  // all output so far (untranslated history)
    uint32_t r0_ = 1, r1_ = 1, r2_ = 1;
    bool header_read_ = false;
    int32_t intel_filesize_ = 0;
    int block_type_ = BlockInvalid;
    size_t block_remaining_ = 0;
    size_t block_length_ = 0;

    void read_lengths(uint8_t* lens, int first, int last) {
        uint8_t pre_len[kPretreeSymbols];
        for (int i = 0; i < kPretreeSymbols; ++i) pre_len[i] = static_cast<uint8_t>(bits_.read(4));
        Tree pre;
        pre.build(pre_len, kPretreeSymbols);
        int x = first;
        while (x < last) {
            int z = pre.decode(bits_);
            if (z == 17) {
                int y = static_cast<int>(bits_.read(4)) + 4;
                while (y-- && x < last) lens[x++] = 0;
            } else if (z == 18) {
                int y = static_cast<int>(bits_.read(5)) + 20;
                while (y-- && x < last) lens[x++] = 0;
            } else if (z == 19) {
                int y = static_cast<int>(bits_.read(1)) + 4;
                int v = pre.decode(bits_);
                if (v > 16) throw ParseError("LZX: invalid pretree run");
                int nl = lens[x] - v;
                if (nl < 0) nl += 17;
                while (y-- && x < last) lens[x++] = static_cast<uint8_t>(nl);
            } else {
                int nl = lens[x] - z;
                if (nl < 0) nl += 17;
                lens[x++] = static_cast<uint8_t>(nl);
            }
        }
    }

    void start_block() {
        block_type_ = static_cast<int>(bits_.read(3));
        uint32_t hi = bits_.read(16);
        uint32_t lo = bits_.read(8);
        block_remaining_ = block_length_ = (static_cast<size_t>(hi) << 8) | lo;
        switch (block_type_) {
            case BlockAligned: {
                uint8_t al[kAlignedSymbols];
                for (auto& a : al) a = static_cast<uint8_t>(bits_.read(3));
                aligned_.build(al, kAlignedSymbols);
            }
                [[fallthrough]];
            case BlockVerbatim:
                read_lengths(main_len_.data(), 0, kNumChars);
                read_lengths(main_len_.data(), kNumChars, main_symbols_);
                main_.build(main_len_.data(), main_symbols_);
                read_lengths(length_len_.data(), 0, kLengthSymbols);
                length_.build(length_len_.data(), kLengthSymbols);
                break;
            case BlockUncompressed: {
                // Align to 16 bits: discard 1..16 bits
                int drop = bits_.left() & 15;
                if (drop == 0) drop = 16;
                bits_.ensure(drop);
                bits_.remove(drop);
                bits_.reset_to_bytes();
                auto u32 = [&]() {
                    uint32_t v = in_.byte();
                    v |= static_cast<uint32_t>(in_.byte()) << 8;
                    v |= static_cast<uint32_t>(in_.byte()) << 16;
                    v |= static_cast<uint32_t>(in_.byte()) << 24;
                    return v;
                };
                r0_ = u32();
                r1_ = u32();
                r2_ = u32();
                break;
            }
            default: throw ParseError("LZX: invalid block type");
        }
    }

    void copy_match(size_t offset, size_t length) {
        if (offset == 0 || offset > window_.size()) throw ParseError("LZX: match offset out of range");
        if (offset > window_size_) throw ParseError("LZX: match offset beyond window");
        if (window_.size() + length > out_size_ + 257) throw ParseError("LZX: match runs past end of output");
        size_t from = window_.size() - offset;
        for (size_t i = 0; i < length; ++i) window_.push_back(window_[from + i]);
    }

    void decode_until(size_t frame_end) {
        while (window_.size() < frame_end) {
            if (block_remaining_ == 0) {
                if (block_type_ == BlockUncompressed) {
                    if (block_length_ & 1) in_.byte();
                    block_type_ = BlockInvalid;  // padding consumed
                    block_length_ = 0;
                }
                start_block();
            }
            size_t todo = frame_end - window_.size();
            size_t run = std::min(block_remaining_, todo);
            if (block_type_ == BlockUncompressed) {
                for (size_t i = 0; i < run; ++i) window_.push_back(in_.byte());
                block_remaining_ -= run;
                continue;
            }
            // Verbatim / aligned
            long long this_run = static_cast<long long>(run);
            while (this_run > 0) {
                int main_el = main_.decode(bits_);
                if (main_el < kNumChars) {
                    window_.push_back(static_cast<uint8_t>(main_el));
                    --this_run;
                    continue;
                }
                main_el -= kNumChars;
                size_t match_len = static_cast<size_t>(main_el & 7);
                if (match_len == 7) match_len += static_cast<size_t>(length_.decode(bits_));
                match_len += 2;
                int slot = main_el >> 3;
                uint32_t offset;
                if (slot > 2) {
                    int extra = extra_bits_[static_cast<size_t>(slot)];
                    offset = position_base_[static_cast<size_t>(slot)] - 2;
                    if (block_type_ == BlockAligned) {
                        if (extra > 3) {
                            offset += bits_.read(extra - 3) << 3;
                            offset += static_cast<uint32_t>(aligned_.decode(bits_));
                        } else if (extra == 3) {
                            offset += static_cast<uint32_t>(aligned_.decode(bits_));
                        } else if (extra > 0) {
                            offset += bits_.read(extra);
                        } else {
                            offset = 1;
                        }
                    } else {
                        if (extra > 0)
                            offset += bits_.read(extra);
                        else
                            offset = 1;
                    }
                    r2_ = r1_;
                    r1_ = r0_;
                    r0_ = offset;
                } else if (slot == 0) {
                    offset = r0_;
                } else if (slot == 1) {
                    offset = r1_;
                    r1_ = r0_;
                    r0_ = offset;
                } else {
                    offset = r2_;
                    r2_ = r0_;
                    r0_ = offset;
                }
                copy_match(offset, match_len);
                this_run -= static_cast<long long>(match_len);
            }
            // A match may overshoot the frame; it still consumes block bytes
            size_t used = static_cast<size_t>(static_cast<long long>(run) - this_run);
            if (used > block_remaining_) throw ParseError("LZX: match overran block");
            block_remaining_ -= used;
        }
    }

    void e8_translate(uint8_t* data, size_t size, size_t frame_start) {
        int32_t filesize = intel_filesize_;
        size_t i = 0;
        int32_t curpos = static_cast<int32_t>(frame_start);
        while (i + 10 < size) {
            if (data[i] != 0xE8) {
                ++i;
                ++curpos;
                continue;
            }
            int32_t abs_off = static_cast<int32_t>(rd32(data + i + 1));
            if (abs_off >= -curpos && abs_off < filesize) {
                int32_t rel = abs_off >= 0 ? abs_off - curpos : abs_off + filesize;
                uint32_t u = static_cast<uint32_t>(rel);
                data[i + 1] = static_cast<uint8_t>(u);
                data[i + 2] = static_cast<uint8_t>(u >> 8);
                data[i + 3] = static_cast<uint8_t>(u >> 16);
                data[i + 4] = static_cast<uint8_t>(u >> 24);
            }
            i += 5;
            curpos += 5;
        }
    }
};

}  // namespace

Buffer lzx_decompress(const std::vector<std::pair<const uint8_t*, size_t>>& blocks, int window_bits, size_t out_size) {
    Input in(blocks);
    Decoder d(in, window_bits, out_size);
    return d.run();
}

}  // namespace oneconv::cab
