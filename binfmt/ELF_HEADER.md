# The ELF header

Specs: [gABI](https://gabi.xinuos.com/v42/index.html) · [elf(5)](https://man7.org/linux/man-pages/man5/elf.5.html)

The ELF header sits at offset 0 of every ELF file. It is a small fixed-size
table that says what the file is and where everything else in it lives.

There are only two layouts, `Elf32_Ehdr` (52 bytes) and `Elf64_Ehdr`
(64 bytes). Both have the same fields in the same order.

---

## Part 1: `e_ident`, the first 16 bytes (`EI_NIDENT`)

Plain single bytes, so they read the same on every machine. You read these
**before** you know anything else about the file, and they tell you how to
read the rest.

| Byte | Index name      | Values                                                                 |
|------|-----------------|------------------------------------------------------------------------|
| 0–3  | `EI_MAG0..3`    | `7f 45 4c 46` (`\x7f E L F`)                                           |
| 4    | `EI_CLASS`      | `ELFCLASS32` = 1, `ELFCLASS64` = 2                                     |
| 5    | `EI_DATA`       | `ELFDATA2LSB` = 1 (little-endian), `ELFDATA2MSB` = 2 (big-endian)      |
| 6    | `EI_VERSION`    | `EV_CURRENT` = 1                                                       |
| 7    | `EI_OSABI`      | `ELFOSABI_SYSV` = 0, `ELFOSABI_GNU`/`ELFOSABI_LINUX` = 3, `ELFOSABI_FREEBSD` = 9, … |
| 8    | `EI_ABIVERSION` | usually 0                                                              |
| 9–15 | `EI_PAD`        | zeros, ignore                                                          |

---

## Part 2: the rest of the header

From byte 16 on, every multi-byte field is in the byte order `EI_DATA` set.

Type sizes:

| Type   | Elf32 | Elf64 |
|--------|-------|-------|
| `Half` | 2     | 2     |
| `Word` | 4     | 4     |
| `Addr` | 4     | **8** |
| `Off`  | 4     | **8** |

| Field         | Type   | Offset 32 / 64 | Meaning                                                        |
|---------------|--------|----------------|----------------------------------------------------------------|
| `e_type`      | `Half` | 16 / 16        | file kind (see below)                                          |
| `e_machine`   | `Half` | 18 / 18        | CPU (see below)                                                |
| `e_version`   | `Word` | 20 / 20        | `EV_CURRENT` = 1                                               |
| `e_entry`     | `Addr` | 24 / 24        | **virtual address** where execution starts, 0 if none         |
| `e_phoff`     | `Off`  | 28 / 32        | file offset of the program header table, 0 if none            |
| `e_shoff`     | `Off`  | 32 / 40        | file offset of the section header table, 0 if none            |
| `e_flags`     | `Word` | 36 / 48        | CPU-specific flags                                             |
| `e_ehsize`    | `Half` | 40 / 52        | size of this header: 52 / 64                                   |
| `e_phentsize` | `Half` | 42 / 54        | size of one program header: 32 / 56                           |
| `e_phnum`     | `Half` | 44 / 56        | number of program headers                                      |
| `e_shentsize` | `Half` | 46 / 58        | size of one section header: 40 / 64                            |
| `e_shnum`     | `Half` | 48 / 60        | number of section headers                                      |
| `e_shstrndx`  | `Half` | 50 / 62        | index of the section that holds section *names*               |
| (end)         |        | 52 / 64        |                                                                |

### `e_type`

| Name       | Value         | What                                                  |
|------------|---------------|-------------------------------------------------------|
| `ET_NONE`  | 0             | no type                                               |
| `ET_REL`   | 1             | relocatable object (`.o`)                             |
| `ET_EXEC`  | 2             | executable at a fixed address (`-no-pie`)             |
| `ET_DYN`   | 3             | shared object: a `.so` **or** a PIE executable        |
| `ET_CORE`  | 4             | core dump                                             |
| OS range   | 0xfe00–0xfeff | `ET_LOOS`..`ET_HIOS`                                  |
| CPU range  | 0xff00–0xffff | `ET_LOPROC`..`ET_HIPROC`                              |

### `e_machine` (the common ones)

| Name         | Value      |
|--------------|------------|
| `EM_386`     | 3          |
| `EM_MIPS`    | 8          |
| `EM_ARM`     | 40         |
| `EM_X86_64`  | 62         |
| `EM_AARCH64` | 183 (0xb7) |
| `EM_RISCV`   | 243        |

---

## Does the header change per architecture, word size, etc.?

- **32 vs 64-bit:** there are two layouts. The fields are the same; only
  `e_entry`, `e_phoff` and `e_shoff` grow from 4 to 8 bytes, which moves
  every field after them.
- **Endianness:** the layout stays the same; only how you decode integers
  changes. That gives four real combinations: 32-LE, 32-BE, 64-LE, 64-BE.
- **CPU:** there is no per-CPU header. The CPU is just the `e_machine` value.
  What changes per CPU is the meaning of `e_flags` (ARM keeps its EABI version
  there, RISC-V its float ABI, and so on), plus things deeper in the file
  such as relocation types. Each architecture documents these in its own
  "psABI supplement" to the gABI.
- **OS:** just the `EI_OSABI` byte. Linux binaries often have 0 there.

Don't infer the class from the machine. x32 is `ELFCLASS32` with
`EM_X86_64`, and MIPS comes in both classes.

---

## Parsing order

1. Make sure the file is at least `EI_NIDENT` (16) bytes, then read `e_ident`.
2. Check the magic.
3. Pick the layout from `EI_CLASS` and the byte order from `EI_DATA`.
4. Read the rest of the header with that layout and byte order.
5. Validate before trusting any of it:
   - `e_ehsize` matches the expected size for the class.
   - `e_phentsize` / `e_shentsize` match the expected entry sizes (when the
     table exists).
   - `offset + count * entsize` fits inside the file. Check this without
     overflowing: `count <= (size - offset) / entsize`.

---

## Gotchas

- `e_shoff` can be 0, meaning the file has no section headers. A loader only
  needs program headers, so a runnable binary may carry no section headers at all.
- `e_shstrndx` can be `SHN_UNDEF` (0), meaning there are no section names.
- **Extended numbering** (rare but legal). When the real value doesn't fit
  in a `Half`, it lives in section header #0 instead:

  | Header says                        | Real value is in      |
  |------------------------------------|-----------------------|
  | `e_phnum == PN_XNUM` (0xffff)      | `sh_info` of section 0 |
  | `e_shnum == 0` and `e_shoff != 0`  | `sh_size` of section 0 |
  | `e_shstrndx == SHN_XINDEX` (0xffff)| `sh_link` of section 0 |

- Every offset and count in here came from the file. A hostile file lies.

---

## Example: x86-64 PIE, 64-bit little-endian

```
00: 7f 45 4c 46 02 01 01 00  00 00 00 00 00 00 00 00
10: 03 00 3e 00 01 00 00 00  50 10 00 00 00 00 00 00
20: 40 00 00 00 00 00 00 00  a8 36 00 00 00 00 00 00
30: 00 00 00 00 40 00 38 00  0d 00 40 00 1f 00 1e 00
```

| Bytes      | Field         | Value                                   |
|------------|---------------|-----------------------------------------|
| 0x00–0x03  | magic         | `\x7fELF`                               |
| 0x04       | `EI_CLASS`    | 2 → 64-bit                              |
| 0x05       | `EI_DATA`     | 1 → little-endian                       |
| 0x06       | `EI_VERSION`  | 1                                       |
| 0x07       | `EI_OSABI`    | 0 → System V                            |
| 0x10       | `e_type`      | 3 → `ET_DYN` (PIE here)                 |
| 0x12       | `e_machine`   | 0x3e = 62 → x86-64 (aarch64: `b7 00`)   |
| 0x14       | `e_version`   | 1                                       |
| 0x18       | `e_entry`     | 0x1050                                  |
| 0x20       | `e_phoff`     | 0x40 → right after this header          |
| 0x28       | `e_shoff`     | 0x36a8                                  |
| 0x30       | `e_flags`     | 0                                       |
| 0x34       | `e_ehsize`    | 64                                      |
| 0x36       | `e_phentsize` | 56                                      |
| 0x38       | `e_phnum`     | 13                                      |
| 0x3a       | `e_shentsize` | 64                                      |
| 0x3c       | `e_shnum`     | 31                                      |
| 0x3e       | `e_shstrndx`  | 30                                      |

To check against a real file on the VM: `xxd -l 64 ./binfmt` and `readelf -h ./binfmt`.
