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
make build NVRM_SRC=/path/to/nvidia-gb10-freebsd
make test
```

`make build` builds libnvrm from `NVRM_SRC/lib/libnvrm`, then NVK with
`-Dvulkan-drivers=nvk -Dnvk-kmd=nvrm`, then the probe. NVK and its ICD are
installed under `WORK/prefix`. `make test` runs on libnvrm's **fake backend**
(`NVRM_BACKEND=fake`, profiles `discrete` and `unified`), so it needs no GPU
and no nvidia driver. It also fails if an installed file name, the installed
ICD or the program output says "nouveau".

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
