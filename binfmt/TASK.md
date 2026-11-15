# Task: Read a Binary the Way the Loader Does — then Become It

Two phases, one codebase.

- **Phase 1 — the parser.** Given an executable, report what it *is*: its
  segments, its sections, the symbols it defines, the libraries it imports,
  and where execution begins — without calling a single library that parses
  the format for you.
- **Phase 2 — the loader.** Take a statically-linked executable, map it into
  memory yourself, build the stack the C runtime expects, and jump to its
  entry point so it *runs* — no `execve`, no help from the dynamic linker.
  You re-implement the inner loop of what the kernel does on `exec`.

Phase 1 teaches you to read a binary. Phase 2 makes you the thing that runs
one. Together they are the literacy and the mechanics under everything
downstream — debuggers, disassembly, exploitation, every tool that has to
know where code lives and how it got there.

**Linux only, both phases. ELF only.** Build and run inside a Linux VM
(aarch64 is fine — it's what you've got). Don't spend a line on Mach-O or
macOS; it's a different format with a different rabbit hole, and it's not
where you're headed.

```
cc -std=c11 -Wall -Wextra -Werror binfmt.c -o binfmt
```

No `-l` anything. `<elf.h>` gives you the struct definitions and the
constants; that is allowed and expected. Anything that *walks the file for
you* is not.

---
---

# Phase 1 — The parser

## Exact inventory

```
1 program, 1 file, no threads, no forks.

input:  a path to a 64-bit ELF executable or shared library
output: a report on stdout, four sections:
          1. header   — magic, arch, file type, entry point
          2. segments — program headers: type, vm range, file range, perms
          3. sections — section headers: name, type, size
          4. imports  — symbol table summary + list of needed libraries
```

One binary in, one report out. A single positional argument; no flags
required. The thing you are re-implementing, piece by piece, is
`readelf -a`. When your output lines up with `readelf` on your own
binaries, Phase 1 is done.

---

## What you build

**Map the file.** `open`, `fstat` for the length, `mmap` it `PROT_READ`.
Everything after this is pointer arithmetic over that mapping. You never
`read()` a struct into a buffer — you cast a pointer onto bytes the kernel
already placed in your address space. You have done this before: the event
loop cast `struct signalfd_siginfo` onto bytes off an fd. Same move, new
target.

**Check `e_ident` before you trust anything.** Magic (`\x7f E L F`), class
(`ELFCLASS64`), byte order. Bail cleanly if it isn't a 64-bit ELF.

**Walk the two tables, and understand why there are two.**

ELF ships *two* descriptions of the same bytes:

- **Program headers** (`e_phoff`, `Elf64_Phdr`) — the **loader's** view.
  `PT_LOAD` segments to map, `PT_INTERP` naming the dynamic linker,
  `PT_DYNAMIC` pointing at the dynamic wiring. This is the table Phase 2
  lives on.
- **Section headers** (`e_shoff`, `Elf64_Shdr`) — the **linker's** view.
  `.text`, `.data`, `.symtab`, `.dynsym`, `.rodata`. Finer grained, and
  strippable: a shipped binary may carry none.

Map those onto the four report sections:

- **header** — `e_type` (watch out: a PIE executable is `ET_DYN`, same as a
  shared library), `e_machine`, and `e_entry` — the entry point, as a
  **virtual address**, not a file offset.
- **segments** — the `PT_LOAD` program headers with their `p_flags`
  permissions (R/W/X), their `p_vaddr`/`p_memsz` in memory and
  `p_offset`/`p_filesz` in the file. Notice when `p_memsz > p_filesz`:
  that gap is `.bss`, and it is not in the file.
- **sections** — the section headers. Section *names* are themselves
  indices into a dedicated string-table section (`e_shstrndx`); resolving
  them teaches you ELF's whole by-reference naming scheme.
- **imports** — walk `.dynamic` for `DT_NEEDED` entries (each is an offset
  into `.dynstr`) to list the needed libraries, and summarize `.symtab` /
  `.dynsym`: how many symbols, how many global, how many undefined.
  Undefined symbols are the ones the dynamic linker fills in at load time.

A symbol-table entry is tiny and fixed-size; the variable-length name lives
elsewhere, reached by an index. That indirection is a pattern you will see
in every format ever designed — and a place out-of-range indices become
bugs.

---

## Output (Phase 1)

Plain text, aligned columns, one fact per line. No JSON, no ncurses. The bar
is: a human reads your output next to `readelf -lSd` and agrees they
describe the same binary. Addresses and offsets in hex, sizes in decimal or
hex — pick one and keep it.

---

## The constraint that makes this real (Phase 1)

**Treat the file as hostile. Every offset you read came from inside the
file, and a malicious file lies.** Before you dereference anything — a
program header, a section header, a string pointer, a symbol entry — the
byte range you are about to touch must be proven to sit inside
`[base, base + filesize)`. A truncated file, an `e_phoff` past the end, an
`e_shnum` that runs the table off the map, an `sh_name` past the end of the
string table, a `DT_NEEDED` offset into nowhere: each must produce a clean
error, never a crash and never a read past the mapping.

This is not busywork bolted onto a parser. Untrusted-input parsing *is* the
parser — malformed-file handling is a huge share of real CVEs, and you are
here for exactly that groundwork. Write a two-line `bounds_ok(p, n)` helper
early and route every dereference through it, or you will forget one, and
the one you forgot is the bug.

---

## Acceptance tests (Phase 1)

1. `./binfmt ./binfmt` — the tool parses itself. Entry point, `PT_LOAD`
   segments, sections, and `libc` in the imports all appear.
2. Your segments and sections match `readelf -lS ./binfmt` — same names,
   same addresses, same count. Diff them.
3. Your import list matches `readelf -d ./binfmt` exactly.
4. Point it at `/bin/ls` — a binary you did not compile. It parses.
5. **Truncate a copy** — `head -c 200 binfmt > broken` — and run it. Clean
   error, no crash, no out-of-bounds read.
6. **Corrupt a length field** — flip `e_shnum` or `e_phnum` to something
   huge with a hex editor — and run it. Clean error, no runaway loop.
7. Clean under `valgrind` and under AddressSanitizer (`-fsanitize=address`).
   Tests 5 and 6 are the ones ASan earns its keep on — run them under it
   specifically.

Tests 1–4 prove you can read a well-formed file; anyone can do that. Tests
5–6 prove you wrote a *parser* and not a demo.

---
---

# Phase 2 — Become the loader

Reuse the parser. Now, instead of describing a static binary, *run* it:
build its address space, hand it a stack, and transfer control. The kernel
gives you a blank `mmap` arena and nothing else — no `execve`, no `ld.so`.
Everything the C runtime finds when it wakes up, you put there.

**Static only, and position-dependent first.** A dynamically-linked binary
needs `ld.so` (its `PT_INTERP`), symbol resolution, and PLT/GOT — a whole
second program you are not writing yet. A statically-linked one has no
interpreter: the kernel maps it and jumps, and so do you. Within static,
`ET_EXEC` (`-static -no-pie`) wants its segments at fixed addresses and
needs no relocations — start there. Earn your way to static-PIE afterward.

---

## What you build (the loader)

1. **Map the segments.** For each `PT_LOAD`: `mmap` a region covering
   `[p_vaddr, p_vaddr + p_memsz)` rounded to pages, copy `p_filesz` bytes
   from `p_offset`, and leave the `p_memsz - p_filesz` tail as zeroes —
   that's `.bss`. Set protections from `p_flags` (`PROT_READ`/`WRITE`/
   `EXEC`). For `ET_EXEC` you `MAP_FIXED` at the exact `p_vaddr`; for
   `ET_DYN` you pick one base (the **load bias**) and add it to every
   address.

2. **Build the initial stack.** Allocate a stack region, then lay down, from
   the stack pointer upward, exactly what the ABI says `_start` will find:

   ```
   sp ->  argc
          argv[0], argv[1], ... , NULL
          envp[0], ...           , NULL
          auxv: (type,val) pairs  , AT_NULL
          ... the actual argv/envp strings, and 16 random bytes for AT_RANDOM
   ```

   `sp` must be 16-byte aligned at entry. The auxiliary vector is the part
   nobody documents well and the part that makes static glibc actually run:
   at minimum `AT_PAGESZ`, `AT_RANDOM` (glibc reads 16 bytes here for the
   stack canary and pointer guard — omit it and you crash), `AT_PHDR`/
   `AT_PHENT`/`AT_PHNUM`, `AT_ENTRY`, terminated by `AT_NULL`.

3. **Transfer control.** You cannot call `e_entry` as a C function pointer —
   the ABI at process entry is different: nothing meaningful in registers,
   everything on the stack, and `sp` must be *your* stack, not the one
   you're standing on. A few lines of inline asm: set `sp` to your stack,
   clear the registers (`x0 = 0` — no rtld-fini for a static binary), and
   `br` to the entry (plus load bias). You do not return. The loaded program
   runs to its own `exit_group`, which ends your process — so the loaded
   program's exit status becomes *your* process's exit status. That is your
   observable.

---

## The difficulty ramp (do them in this order)

- **Milestone A — a freestanding blob.** Hand-write a tiny program in asm
  that does `write(1, "hi\n", 3)` and `exit(0)` via `svc #0`, link it
  `-nostdlib -static`. It reads no argv and no auxv, so your loader only has
  to map two segments, set `sp` to *any* valid mapped region, and jump.
  This proves your mapping and your control transfer in isolation, with the
  stack-building complexity switched off.
- **Milestone B — static libc.** `musl-gcc -static -no-pie hello.c` (musl is
  smaller and more forgiving than glibc; use it first). Now `_start` runs
  libc startup, which *does* read argc/argv/envp/auxv — so this is the
  milestone that forces the full stack build from step 2. "hello" must
  print and the process must exit 0.
- **Milestone C — static-PIE.** `ET_DYN`, a nonzero load bias, and a
  `PT_DYNAMIC` with `R_AARCH64_RELATIVE` relocations: for each, write
  `base + r_addend` into `*(base + r_offset)`. No symbol lookup — relative
  relocations are self-contained. This is the smallest possible taste of
  being a dynamic linker, and it's where the load bias stops being a number
  you ignore and becomes a number you *apply*.

---

## The constraint that makes this real (Phase 2)

**No `execve`, no `posix_spawn`, no `ld.so`, no shelling out.** The only way
code from the target file may run is: you mapped it, you built its stack, you
jumped to it. If any part of the real loader does the work for you, you
skipped the lesson. The corollary: when the loaded program is running, there
is no C library of *yours* underneath it — you are not calling it, you handed
the whole process to it. Think about what that means for cleanup (there is
none; it ends by exiting) and for debugging (a wrong stack is a `SIGSEGV` at
`e_entry` with nothing to print).

---

## Acceptance tests (Phase 2)

1. Milestone A: your loader runs the freestanding blob; `hi` appears,
   `echo $?` is 0.
2. Milestone B: your loader runs the static musl hello; `hello` appears,
   exit status is 0. Pass it argv and have the program print `argv[1]` —
   proving your stack is real, not luck.
3. The program's view of its environment matches what you built: have it
   print an env var you placed in the stack, and `getauxval(AT_PAGESZ)`,
   and check both.
4. Milestone C: a `-static-pie` build runs correctly from a nonzero load
   bias. Run it twice with two different biases and confirm both work —
   that's your relocations actually relocating.
5. No `execve`/`execveat`/`clone`-to-exec anywhere: `strace -f ./binfmt
   --exec ./hello` shows your `mmap`/`mprotect` calls and the target's own
   syscalls, but no exec of the target.

---
---

# Stretch goals (either phase, pointing onward)

- **Mitigation report** (Phase 1) — from the headers alone, print whether
  the binary is PIE, whether the stack is non-executable (`PT_GNU_STACK`
  flags), and whether it links the stack-canary machinery (a
  `__stack_chk_fail` import). You are reading, from the outside, the exact
  defenses you will later study from the inside.
- **Disassemble the entry point** (Phase 1) — print the first 16 bytes at
  `e_entry` as hex, then decode one aarch64 instruction by hand. The doorway
  from containers to code.
- **Fork-and-report** (Phase 2) — load the target in a forked child so the
  parent survives to print the child's real exit status and any signal that
  killed it, instead of inheriting it by dying.
- **ASLR, yourself** (Phase 2) — randomize the load bias with
  `getrandom`, and confirm the static-PIE target still runs. You've now
  built the thing ASLR *is*.
- **Dynamic linking** (far Phase 3, a project of its own) — honor
  `PT_INTERP` by loading `ld.so` alongside the target and letting it finish
  the job. Enormous; do not start it until the static loader is boring.
- **Auto-diff harness** — a script that runs your parser and `readelf`
  across a directory of binaries and asserts agreement. A regression test
  and, secretly, a differential fuzzing target.

---

# Self-check

Answer without looking anything up. If you can't, some part was cargo-culted.

**Phase 1**

- ELF carries two tables describing the same bytes — program headers and
  section headers. Who reads each one? Which can you strip and still have a
  program that runs, and why?
- `e_entry` is a virtual address, not a file offset. Given only the file on
  disk, how do you find the *bytes* of the first instruction? Which table
  did you have to consult, and what arithmetic did you do?
- You walk `e_phnum` program headers, or `e_shnum` section headers. A
  crafted file sets one of those counts to `0xFFFF`. What does your loop do?
  Which single check stops it, and where did you put it?
- A symbol entry is fixed-size but its name is arbitrary length and lives in
  a separate string table, reached by an index. Why built that way? What
  breaks if the index is out of range and you didn't check?
- A PIE executable and a shared library are both `ET_DYN`. So what actually
  distinguishes a program from a library on disk, and what does the kernel
  use to decide it can run one?

**Phase 2**

- Draw the stack the instant `_start` begins. Where does libc find `environ`?
  Where does it find `argc`? What is at the very top of the stack, above the
  auxv?
- Why can't you call `e_entry` as a C function pointer? Name two things that
  are true at process entry that are not true at a normal function call.
- The `.bss` isn't in the file. Where did its zeroes come from — you, the
  kernel, or both? Which pages, exactly?
- A single page can hold the end of a read-execute segment and the start of
  a read-write one. When you set protections, what do you owe that shared
  page, and what goes wrong if you get it backward?
- A static binary links no shared libraries. So why does the static-PIE
  version still need relocations at all? What, concretely, is
  `R_AARCH64_RELATIVE` fixing up?
- You chose the load address; the real kernel randomizes it. What did you
  have to do differently for `ET_EXEC` versus `ET_DYN`, and why could you
  `MAP_FIXED` one but not naively the other?
- You removed `AT_RANDOM` from the auxv and a static glibc hello crashed
  before `main`. What did it try to do with those 16 bytes, and why is that
  the same mechanism you'll meet again in the stack-smashing project?
