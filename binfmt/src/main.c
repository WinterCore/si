#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "elf_parser.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Error: Binary file must be provided...");
        exit(EXIT_FAILURE);
    }

    int fd;

    if ((fd = open(argv[1], O_RDONLY)) == -1) {
        perror("Failed to open:");
        exit(EXIT_FAILURE);
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("Failed to fstat:");
        exit(EXIT_FAILURE);
    }
    
    uint8_t *base = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (base == MAP_FAILED) {
        perror("Failed to mmap file:");
        exit(EXIT_FAILURE);
    }

    ElfParser elf_parser = elf_parser_create(base, st.st_size);
    ElfHeader elf_header;
    ElfParseResult parse_result = elf_parser_parse_header(&elf_parser, &elf_header);

    if (parse_result != ELF_PARSE_OK) {
        fprintf(stderr, "Errro: Failed to parse ELF file");
        return 1;
    }

    return 0;
}
