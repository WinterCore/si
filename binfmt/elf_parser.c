
#include "elf_parser.h"
#include "buffer_reader.h"

ElfParser ep_create() {


    return (ElfParser) {
        .br = br_new();
    };
}

void ep_parse_header(ElfParser *parser) {
    br_bytes(BufferReader *r, size_t n);
}
