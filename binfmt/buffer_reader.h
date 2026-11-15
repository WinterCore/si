#ifndef BUFFER_READER_H
#define BUFFER_READER_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct BufferReader {
    const uint8_t *data;
    size_t size;

    // Offset of the next read, always <= size
    size_t pos;

    // Multi-byte reads decode as big-endian when set, little-endian otherwise
    bool big_endian;

    // Sticky: set by the first out-of-bounds access. After that every read
    // returns 0 (or NULL) and pos stops moving, so you can do a run of reads
    // and check this once at the end.
    bool failed;
} BufferReader;

BufferReader br_new(const void *data, size_t size);

// True if [offset, offset + n) sits inside the buffer
bool br_bounds_ok(const BufferReader *r, size_t offset, size_t n);

// Move pos to an absolute offset (offset == size is allowed)
bool br_seek(BufferReader *r, size_t offset);
bool br_skip(BufferReader *r, size_t n);

// Read and advance, in the reader's byte order
uint8_t br_u8(BufferReader *r);
uint16_t br_u16(BufferReader *r);
uint32_t br_u32(BufferReader *r);
uint64_t br_u64(BufferReader *r);

// Pointer to the next n bytes inside the buffer, then advance past them.
// NULL if they don't fit.
const uint8_t *br_bytes(BufferReader *r, size_t n);

// A new reader over [offset, offset + size) of r, starting at pos 0 and
// keeping r's byte order. Comes back already failed if the range isn't
// inside r. Reads from it can't escape the range, so a bad offset into
// e.g. a string table fails instead of reading the next section.
BufferReader br_slice(const BufferReader *r, size_t offset, size_t size);

// The NUL-terminated string starting at offset. NULL if offset is out of
// range or the string runs off the end of the buffer. Doesn't touch pos
// or failed.
const char *br_cstr_at(const BufferReader *r, size_t offset);

#endif
