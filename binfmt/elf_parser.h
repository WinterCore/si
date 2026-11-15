#ifndef ELF_PARSER_H
#define ELF_PARSER_H

#include "buffer_reader.h"
#include <inttypes.h>

typedef struct ElfHeader {
    // 1 = 32-bit, 2 = 64-bit
    uint8_t ei_class;

    // 1 = little-endian, 2 = big-endian
    uint8_t ei_data;
    
    // Always 1
    uint8_t ei_version;

    // 0 = System V (generic), 3 = Linux/GNU, 9 = FreeBSD
    uint8_t ei_osabi;

    // Usually 0
    uint8_t ei_abiversion;
} ElfHeader;

typedef struct ElfParser {
     BufferReader br;
} ElfParser;

ElfParser ep_create();

void ep_parse_header(ElfParser *parser);

#endif
