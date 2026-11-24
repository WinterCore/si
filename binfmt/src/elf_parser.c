#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include "elf_parser.h"

#define read_byte_or_return(parser, out) \
    do { \
        if ((parser)->pos >= (parser)->file_size) { \
            return ELF_UNEXPECTED_EOF; \
        } \
        *(out) = (parser)->data[(parser)->pos]; \
        (parser)->pos += 1; \
    } while (0);

#define parse_ok_or_return(expr) \
    do { \
        ElfParseResult _r = (expr); \
        if (_r != ELF_PARSE_OK) \
            return _r; \
    } while (0);


ElfParser elf_parser_create(uint8_t *data, size_t file_size) {
    return (ElfParser) {
        .pos = 0,
        .data = data,
        .file_size = file_size
    };
}

ElfParseResult elf_parser_read_bytes(
    ElfParser *parser,
    int count,
    uint8_t *out
) {
    for (int i = 0; i < count; i += 1) {
        read_byte_or_return(parser, &out[i]);
    }

    return ELF_PARSE_OK;
}

ElfParseResult elf_parser_parse_ident(ElfParser *parser, ElfIdent *ident) {

    // Validate magic seq
    char magic[4];
    parse_ok_or_return(elf_parser_read_bytes(parser, 4, (uint8_t *) magic));
    static char *MAGIC = "\x7f" "ELF";

    if (memcmp(magic, MAGIC, 4) != 0) {
        return ELF_INVALID_MAGIC_BYTES;
    }

    return ELF_PARSE_OK;
}

ElfParseResult elf_parser_parse_header(ElfParser *parser, ElfHeader *header) {

    parse_ok_or_return(elf_parser_parse_ident(parser, &header->e_ident));

    return ELF_PARSE_OK;
}

