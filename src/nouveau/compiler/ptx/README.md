# libnvptx: the PTX front end of native CUDA

CloudBSD fork of Mesa (Track #2426).  This directory is ours; nothing here
comes from upstream Mesa.

## What it is

`libnvptx` reads a PTX module into a syntax tree.  It is the first part of
our native CUDA pipeline:

```
CUDA C --clang 23--> PTX text --libnvptx--> syntax tree --> NIR --> NAK
```

The NIR and the NAK side are later slices of #2426.  Slice 1, this one, is
the parser: it reads a module, reports the first error with its line and
column, and prints the tree back as canonical PTX text.

## What slice 1 covers

- the module header: `.version`, `.target` (with its flags), `.address_size`;
- the module scope declarations and the functions (`.entry`, `.func`, the
  `.visible`, `.extern` and `.weak` linkage, the parameter and the return
  parameter lists, the `.maxntid`, `.reqntid`, `.minnctapersm`, `.maxnreg`
  and `.pragma` directives);
- the arrays: an array has a size ("name[1024]") or none at all ("name[]"),
  whose size is set at launch or at link time.  The second form is the
  dynamic shared memory of CUDA, which clang writes as
  ".extern .shared .align 16 .b8 smem[];";
- the bodies: the declarations, the labels, the nested scopes (kept in the
  tree, because a declaration of PTX lives in the scope it was made in) and
  the instructions with their guard, their modifiers and their operands;
- the operand forms: registers, special registers, symbols, integers in four
  bases, the float literals, the addresses, the vectors and the sink;
- the errors: an unknown directive, an unsupported version, a missing `;`, an
  initializer and the rest, each with the line and the column.

The literals follow the PTX ISA: an integer literal is signed unless its
magnitude does not fit in `.s64` or it carries the `U` suffix, and a decimal
float literal takes no suffix and is always 64 bits wide (the ISA, section
4.5).  A string keeps the spelling it had in the source, so that a pragma
with an escape in it is written back with the same escape.

A modifier that carries a scope keeps it whole: `ld.global.L1::no_allocate`,
`cp.async.bulk.shared::cluster` and `mma.sync.aligned.kind::mxf4` are read
as one word each, and the modifier is stored as `L1::no_allocate`.  A `::`
that is not followed by an identifier is an error, not a modifier.

A module names its version and its target: a module without `.version` or
without `.target` is refused with a named error.

The tree of a module has a size.  A function may hold `PTX_MAX_STMTS_PER_FUNC`
(65536) statements and a module may hold `PTX_MAX_OPERANDS_PER_MODULE`
(1048576) operands; a module that holds more is refused with a named error.
The limits are above what the PTX of a compiler holds, so that no real
module is refused.

## What slice 1 does not cover

- initializers (`.shared .b8 buf[4] = {0}` is refused on purpose);
- NIR and NAK: later slices of #2426.

## The API

```c
struct ptx_module *ptx_parse(void *mem_ctx, const char *text, size_t len,
                              struct ptx_diag *diag);
void ptx_print(FILE *f, const struct ptx_module *m);
```

`ptx_parse` returns NULL and fills `diag` on the first error, and clears
`diag` when the module is good; `diag` may be NULL.  Everything it allocates
is `ralloc`ed under `mem_ctx`.  `ptx_print` writes canonical PTX text that
`ptx_parse` reads back into an equal module.

## The test

```
meson test -C cloudbsd-build/mesa --suite nvptx --print-errorlogs
```

The test program is `tests/ptx_parse_test.c`; its data is the PTX that our
own clang 23 produced, in `tests/data/`.
