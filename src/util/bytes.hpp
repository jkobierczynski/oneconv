// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Bounds-checked little-endian binary reader and shared byte buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace oneconv {

/// Thrown for any structural problem in the input data.
class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& what) : std::runtime_error(what) {}
};

using Buffer = std::vector<uint8_t>;
using BufferPtr = std::shared_ptr<const Buffer>;

/// A slice of a shared buffer. Used for embedded files and images so that
/// large payloads are never copied while the model is built.
struct Blob {
    BufferPtr buffer;
    size_t offset = 0;
    size_t size = 0;

    Blob() = default;
    Blob(BufferPtr buf, size_t off, size_t len) : buffer(std::move(buf)), offset(off), size(len) {}
    static Blob from_vector(Buffer data) {
        auto ptr = std::make_shared<const Buffer>(std::move(data));
        size_t n = ptr->size();
        return Blob(ptr, 0, n);
    }

    bool empty() const { return !buffer || size == 0; }
    const uint8_t* data() const { return buffer ? buffer->data() + offset : nullptr; }
};

/// Little-endian reader over a window of a shared buffer.
/// Every access is bounds checked and throws ParseError on overrun.
class Reader {
public:
    Reader() = default;
    explicit Reader(BufferPtr buf) : buf_(std::move(buf)) {
        begin_ = 0;
        end_ = buf_ ? buf_->size() : 0;
        pos_ = 0;
    }
    Reader(BufferPtr buf, size_t begin, size_t end) : buf_(std::move(buf)), begin_(begin), end_(end), pos_(begin) {
        if (!buf_ || end_ > buf_->size() || begin_ > end_) throw ParseError("reader window out of range");
    }
    /// Reader over a private copy of bytes.
    static Reader from_bytes(const uint8_t* p, size_t n) {
        return Reader(std::make_shared<const Buffer>(p, p + n));
    }
    static Reader from_bytes(const Buffer& v) { return from_bytes(v.data(), v.size()); }

    size_t remaining() const { return end_ - pos_; }
    /// Position relative to the start of the reader window.
    size_t position() const { return pos_ - begin_; }
    /// Absolute position in the underlying buffer.
    size_t absolute() const { return pos_; }
    size_t size() const { return end_ - begin_; }
    const BufferPtr& buffer() const { return buf_; }
    bool eof() const { return pos_ >= end_; }

    void need(size_t n) const {
        if (n > remaining()) {
            throw ParseError("unexpected end of data (need " + std::to_string(n) + " bytes, have " +
                             std::to_string(remaining()) + ")");
        }
    }
    void skip(size_t n) {
        need(n);
        pos_ += n;
    }
    void seek(size_t relative) {
        if (relative > size()) throw ParseError("seek out of range");
        pos_ = begin_ + relative;
    }
    uint8_t peek_u8() const {
        need(1);
        return (*buf_)[pos_];
    }
    uint8_t u8() {
        need(1);
        return (*buf_)[pos_++];
    }
    uint16_t u16() {
        need(2);
        const uint8_t* p = buf_->data() + pos_;
        pos_ += 2;
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    uint32_t u24() {
        need(3);
        const uint8_t* p = buf_->data() + pos_;
        pos_ += 3;
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    }
    uint32_t u32() {
        need(4);
        const uint8_t* p = buf_->data() + pos_;
        pos_ += 4;
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    /// Copy n bytes out.
    Buffer bytes(size_t n) {
        need(n);
        const uint8_t* p = buf_->data() + pos_;
        pos_ += n;
        return Buffer(p, p + n);
    }
    /// Zero-copy view of the next n bytes.
    Blob blob(size_t n) {
        need(n);
        Blob b(buf_, pos_, n);
        pos_ += n;
        return b;
    }
    const uint8_t* ptr() const { return buf_->data() + pos_; }

    /// Sub-reader for [offset, offset+len) relative to the window start.
    Reader slice(size_t offset, size_t len) const {
        if (offset > size() || len > size() - offset) {
            throw ParseError("slice out of range (offset " + std::to_string(offset) + ", len " +
                             std::to_string(len) + ", size " + std::to_string(size()) + ")");
        }
        return Reader(buf_, begin_ + offset, begin_ + offset + len);
    }
    /// Sub-reader over the next n bytes; advances this reader.
    Reader take(size_t n) {
        need(n);
        Reader r(buf_, pos_, pos_ + n);
        pos_ += n;
        return r;
    }

private:
    BufferPtr buf_;
    size_t begin_ = 0;
    size_t end_ = 0;
    size_t pos_ = 0;
};

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace oneconv
