#ifndef ELF_PARSER_H
#define ELF_PARSER_H

#include <inttypes.h>
#include <stddef.h>

typedef struct ElfIdent {
    uint8_t ei_class;
    uint8_t ei_data;
    uint8_t ei_version;
    uint8_t ei_osabi;
} ElfIdent;

typedef struct ElfHeader {
    ElfIdent e_ident;
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} ElfHeader;

typedef struct ElfParser {
    uint8_t *data;

    size_t pos;

    size_t file_size;
} ElfParser;

ElfParser elf_parser_create(uint8_t *data, size_t file_size);

int elf_parser_read_chars(ElfParser *parser, int count, char *out);

typedef enum ElfParseResult {
    ELF_PARSE_OK = 0,
    ELF_UNEXPECTED_EOF,
    ELF_INVALID_MAGIC_BYTES,
} ElfParseResult;

ElfParseResult elf_parser_parse_ident(ElfParser *parser, ElfIdent *elf_ident);
ElfParseResult elf_parser_parse_header(ElfParser *parser, ElfHeader *elf_header);

#endif
