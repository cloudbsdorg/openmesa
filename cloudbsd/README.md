# cloudbsd/: build and test support of the CloudBSD Mesa fork

See `../FORK-NOTES.md` for what this fork is and why it exists. It is private
and never submitted upstream.

## What is here

| File | Purpose |
|---|---|
| `make.sh` | the logic behind the top-level `Makefile` |
| `preflight.sh` | the tool check (`make preflight`, `JSON=1` for `--json`) |
| `tests/nvk-nvrm-probe.c` | lists Vulkan devices as JSON Lines and tries `vkCreateDevice` |
| `tests/test-make-checks.sh` | checks `make.sh` check-command exit codes with fixture files |

## Build and test (FreeBSD, normal user)

```
make install-deps            # pkg install of the build dependencies (asks first)
make configure-tree          # set the Mesa build tree up with the chosen llvm-config
make build NVRM_SRC=/path/to/nvidia-gb10-freebsd
make test
```

`make.sh` takes one command: `preflight`, `install-deps`, `configure`,
`configure-tree`, `build`, `test`, `no-nouveau`, `no-gallium-nouveau` or
`clean`. `configure-tree` configures only the Mesa build tree: it writes the
native file and runs `meson setup` (with `--reconfigure` when the tree was
set up with another `llvm-config`). `configure` prints the build options and
**configures nothing**: it writes no file and runs no `meson setup`.  Use
`configure-tree` (or `build`, which calls it) to set a tree up.

`make build` builds libnvrm from `NVRM_SRC/lib/libnvrm`, then NVK with
`-Dvulkan-drivers=nvk -Dnvk-kmd=nvrm`, then the probe. NVK and its ICD are
installed under `WORK/prefix`. `make test` runs on libnvrm's **fake backend**
(`NVRM_BACKEND=fake`, profiles `discrete` and `unified`), so it needs no GPU
and no nvidia driver. It also fails if an installed file name, the installed
ICD or the program output says "nouveau".

The build needs **LLVM 19**: meson takes `llvm-config23` when both LLVMs are
installed and then cannot find LLVMSPIRVLib. `make.sh` writes a meson native
file under `WORK` that names the `llvm-config` to use, and puts the
pkg-config directory of that installation first in `PKG_CONFIG_PATH` (the
directory comes from `llvm-config --prefix`, so the two always belong to the
same LLVM). The native file is written at every build, and a build tree that
was set up with another `llvm-config` is reconfigured with it, so the setting
is never ignored. The default is `/usr/local/bin/llvm-config19`.

## Choosing the llvm-config

Give the path in the **environment**:

```
LLVM_CONFIG=/absolute/path/to/llvm-config19 make build NVRM_SRC=/path/to/nvidia-gb10-freebsd
```

The environment is the form to use: the value reaches `make.sh` exactly as it
was written. A value given to make as a variable on the command line
(`make build LLVM_CONFIG=...`) is **expanded by make** before it reaches
`make.sh`, and GNU make and bmake expand it differently, so a path that holds
a `$` or a `$$` does not arrive as it was written. `make.sh` names the
`llvm-config` it chose, so the build says which one it used.

`MK_LLVM_CONFIG` is the same setting for a direct call of `make.sh`
(`MK_LLVM_CONFIG=/path/to/llvm-config19 sh cloudbsd/make.sh build`).
`LLVM_CONFIG` wins over `MK_LLVM_CONFIG`, and every message names the
variable that supplied the value. A variable that is **set but empty** is a
mistake and is refused as one ("LLVM_CONFIG is set but empty; unset it or
give a path"); it is never taken for a variable that was never set.

The value must be an absolute path to an executable file: it travels in the
environment, never in the recipe, and a value that is not a path is refused
before anything is built. The `llvm-config` must report the version `19.x`
or `19.x.y`; one that fails, that says nothing, or that says something that
is not a version is refused.

The selected installation must have a pkg-config directory
(`--prefix/libdata/pkgconfig` or `--libdir/pkgconfig`), and that directory
must hold the `.pc` files the build needs (`LLVMSPIRVLib.pc` and
`libclc.pc`, from the `spirv-llvm-translator-llvm19` and `libclc-llvm19`
packages). They are looked for in that directory alone, with
`PKG_CONFIG_LIBDIR` naming it and `PKG_CONFIG_PATH` cleared, so that the
package of another installation is never used: when the directory exists but
holds none of them, the build fails instead of taking the package of the
caller, which may belong to another LLVM. Every error names what was asked
of the `llvm-config`, what came back and what to do next.

Only the commands that build Mesa check the `llvm-config`: `configure`,
`configure-tree`, `build` and `test`. `install-deps`, `preflight`, `clean`
and the usage work on a host that has no LLVM at all, so that LLVM 19 can be
installed in the first place.

## nvk-nvrm-probe

```
VK_ICD_FILENAMES=WORK/prefix/share/vulkan/icd.d/nvk_nvrm_icd.aarch64.json \
NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 nvk-nvrm-probe
```

`WORK` is the build directory. The machine architecture is part of the ICD
file name: `aarch64` on arm64 and `x86_64` on amd64.

It prints one JSON object per physical device (schema `nvk-nvrm-probe/1`) with
the fields `vulkaninfo --summary` shows (API and driver version, type, driver
id, name and info, vendor and device id, queue families, memory heaps and
types), then the exact `vkCreateDevice` result and the stub's expected log
line. Exit status: 0 when the devices were probed, 1 when no instance could
be made, and 2 when there is no device.

`vulkaninfo --summary` itself creates a device before it prints anything, so
on the stub it stops at the refusal. It works once libnvrm S2 to S4 land.

## The stub

The nvkmd `nvrm` backend (`src/nouveau/vulkan/nvkmd/nvrm/`) enumerates GPUs
through libnvrm and reports their properties. `vkCreateDevice` returns
`VK_ERROR_FEATURE_NOT_PRESENT` and logs the line in `nvkmd_nvrm_msg.h`.
Memory, VA and submission arrive with libnvrm S2 (#2419), S3 (#2437) and S4
(#2438). Design: nvidia-gb10-freebsd `mesa/nvk-nvrm/DESIGN.md` (#2379).
