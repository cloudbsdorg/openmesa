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
as one word each, and the modifier is stored as `L1::no_allocate`.  The scope
that follows a `::` is an identifier: it starts with a letter or an
underscore, and it may hold digits after that, so `L1::123` is a number and
not a modifier and is refused.  A `::` that follows no identifier of the word
names no scope and is refused too.

A module names its version and its target: a module without `.version` or
without `.target` is refused with a named error.

The tree of a module has a size, and every limit says what it counts:

- `PTX_MAX_STMTS_PER_FUNC` (65536) counts the statements of **one function**:
  the count starts again at every function, so a module of many functions may
  hold more statements than that in all;
- `PTX_MAX_OPERANDS_PER_MODULE` (1048576) counts the instruction operands of
  **the whole module**: that count never starts again;
- `PTX_MAX_MODULE_NODES` (524288) counts the nodes of the tree of **the whole
  module**: a function, a module declaration, a parameter of a function, a
  statement (an instruction, a declaration or a label) and a nested scope.  The
  count is made before the node is allocated, so that a module whose tree
  would grow without bound is refused before the memory is taken;
- `PTX_MAX_SCOPE_DEPTH` (64) is the depth of the nesting of the scopes of a
  body.  The parser reads a nested scope with a recursive call, so that depth
  is the depth of the C stack that the parse uses.

A module that holds more than a limit is refused with a named error: "the
function holds more than 65536 statements", "the module holds more than
1048576 operands", "the module holds more than 524288 nodes" and "the scopes
are nested more than 64 deep".  The limits are above what the PTX of a
compiler holds, so that no real module is refused.

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
