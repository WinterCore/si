#include "buffer_reader.h"

#include <string.h>

BufferReader br_new(const void *data, size_t size) {
    return (BufferReader) { .data = data, .size = size };
}

bool br_bounds_ok(const BufferReader *r, size_t offset, size_t n) {
    // Subtract instead of checking offset + n <= size, which a huge
    // offset or n from the file can wrap around
    return offset <= r->size && n <= r->size - offset;
}

bool br_seek(BufferReader *r, size_t offset) {
    if (r->failed || offset > r->size) {
        r->failed = true;
        return false;
    }

    r->pos = offset;
    return true;
}

const uint8_t *br_bytes(BufferReader *r, size_t n) {
    if (r->failed || !br_bounds_ok(r, r->pos, n)) {
        r->failed = true;
        return NULL;
    }

    const uint8_t *p = r->data + r->pos;
    r->pos += n;
    return p;
}

bool br_skip(BufferReader *r, size_t n) {
    return br_bytes(r, n) != NULL;
}

// Assemble n bytes into an integer one byte at a time, so it works the same
// whatever the host's byte order and wherever the bytes are aligned
static uint64_t read_uint(BufferReader *r, size_t n) {
    const uint8_t *p = br_bytes(r, n);

    if (p == NULL) {
        return 0;
    }

    uint64_t value = 0;

    // Shift in the most significant byte first: that's p[0] for
    // big-endian and p[n - 1] for little-endian
    for (size_t i = 0; i < n; i++) {
        size_t index = r->big_endian ? i : n - 1 - i;
        value = (value << 8) | p[index];
    }

    return value;
}

uint8_t br_u8(BufferReader *r) {
    return (uint8_t)read_uint(r, 1);
}

uint16_t br_u16(BufferReader *r) {
    return (uint16_t)read_uint(r, 2);
}

uint32_t br_u32(BufferReader *r) {
    return (uint32_t)read_uint(r, 4);
}

uint64_t br_u64(BufferReader *r) {
    return read_uint(r, 8);
}

BufferReader br_slice(const BufferReader *r, size_t offset, size_t size) {
    BufferReader slice = { .big_endian = r->big_endian };

    if (r->failed || !br_bounds_ok(r, offset, size)) {
        slice.failed = true;
        return slice;
    }

    slice.data = r->data + offset;
    slice.size = size;
    return slice;
}

const char *br_cstr_at(const BufferReader *r, size_t offset) {
    if (r->failed || offset >= r->size) {
        return NULL;
    }

    const uint8_t *start = r->data + offset;

    if (memchr(start, '\0', r->size - offset) == NULL) {
        return NULL;
    }

    return (const char *)start;
}
