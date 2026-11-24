# ELF Header (Elf64_Ehdr)

The first 64 bytes of every 64-bit ELF file. Everything else in the file is
located by offsets stored *here* — the header is the root of the format.
Defined in `<elf.h>`; the on-disk layout is fixed, no padding.

```
offset  size  field         what it is
------  ----  ------------  ------------------------------------------
0x00    16    e_ident[16]   magic + class + endianness + OSABI (see below)
0x10     2    e_type        file type: ET_EXEC=2, ET_DYN=3, ET_CORE=4
                            (PIE executables are ET_DYN — same as .so!)
0x12     2    e_machine     ISA: EM_AARCH64=183 (0xB7), EM_X86_64=62
0x14     4    e_version     must be 1 (EV_CURRENT)
0x18     8    e_entry       virtual address of the entry point

0x20     8    e_phoff       file offset of the program header table
0x28     8    e_shoff       file offset of the section header table
0x30     4    e_flags       arch-specific flags (usually 0)
0x34     2    e_ehsize      size of this header (64)
0x36     2    e_phentsize   size of one program header entry (56)
0x38     2    e_phnum       number of program header entries
0x3a     2    e_shentsize   size of one section header entry (64)
0x3c     2    e_shnum       number of section header entries
0x3e     2    e_shstrndx    section index of the section-name string table
```

## e_ident[16]

Parsed before anything else — it tells you how to even read the rest.

```
index  name       value          meaning
-----  ---------  -------------  ----------------------------------------
0..3   EI_MAG0-3  7f 45 4c 46    \x7f 'E' 'L' 'F' — reject if no match
4      EI_CLASS   1 / 2          1 = ELFCLASS32, 2 = ELFCLASS64
5      EI_DATA    1 / 2          1 = little-endian, 2 = big-endian
6      EI_VERSION 1              must be 1
7      EI_OSABI   0 / 3          0 = SysV, 3 = Linux (mostly ignored)
8..15  padding    0              ignored
```

## Parsing order

1. File at least 64 bytes? If not: truncated, bail.
2. Magic bytes match? If not: not ELF, bail.
3. `e_ident[4] == 2` (ELFCLASS64)? This tool only does 64-bit.
4. `e_ident[5]`: if 2, the multi-byte fields above are big-endian and must
   be byte-swapped (fields *inside* e_ident are raw bytes, no swap).

## Hostile-file traps (all offsets above come from the file and lie)

- `e_phoff + e_phnum * e_phentsize` must fit inside the file, or bail.
- Same for `e_shoff + e_shnum * e_shentsize`.
- Three escape hatches for tables that outgrow the 16-bit counts:
  - `e_phnum == 0xffff` (PN_XNUM): real count is in `sh_info` of
    section header 0 — which itself must be bounds-checked.
  - `e_shnum == 0` **and** `e_shoff != 0`: real count is in `sh_size`
    of section header 0.
  - `e_shstrndx == 0xffff` (SHN_XINDEX): real index is in `sh_link`
    of section header 0.
- `e_shoff == 0` legitimately means "no section headers" (stripped
  binary) — that's a normal path, not an error.

## How to find the entry point's bytes

`e_entry` is a **virtual address**, not a file offset. To get bytes:
find the `PT_LOAD` segment where `p_vaddr <= e_entry < p_vaddr + p_filesz`,
then file offset = `e_entry - p_vaddr + p_offset`. That arithmetic is why
you need the program header table, not just this header.
