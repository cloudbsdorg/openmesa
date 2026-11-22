# CloudBSD fork of Mesa: private, not for upstream

This repository (`cloudbsdorg/mesa-cloudbsd`) is CloudBSD's **private fork** of
Mesa. It is not a Mesa project repository, and nothing in it is submitted
upstream. No merge request, issue or comment goes to the Mesa GitLab from this
work.

## Branches

| Branch | Content |
|---|---|
| `upstream/26.3.1` | Mesa 26.3.1 exactly as released (tag `mesa-26.3.1`, history from gitlab.freedesktop.org/mesa/mesa). Never changed. |
| `cloudbsd/26.3` | Our work on top of 26.3.1. |

About `src/nouveau`: the directory name is Mesa's; nothing here uses the
nouveau kernel driver. The directory keeps its name so that rebasing on
upstream stays cheap.

## Agent instructions

`AGENTS.md` at the top of the tree is **ours**, and `CLAUDE.md` and
`GEMINI.md` link to it, the same way upstream links them. Mesa's own text is
kept byte for byte in `UPSTREAM-AGENTS.md`. `UPSTREAM-CLAUDE.md` and
`UPSTREAM-GEMINI.md` are byte-identical copies of it: upstream they are links,
but our commit gate does not scan links.

## Why we fork

- **NVK without nouveau.** CloudBSD runs NVIDIA GPUs on NVIDIA's open RM
  (`nvidia.ko`) on FreeBSD, with no nouveau kernel driver and no DRM render
  node. NVK reaches the kernel through its nvkmd layer, and the only upstream
  nvkmd backend is the nouveau one. This fork adds an nvkmd backend, `nvrm`,
  that talks to the RM through our `libnvrm` (nvidia-gb10-freebsd,
  `lib/libnvrm`). The design is `mesa/nvk-nvrm/DESIGN.md` in that repository
  (Track #2379).
- **Native CUDA.** The native CUDA work compiles PTX through NAK
  (`src/nouveau/compiler`), and it needs NAK changes (Track #2380).
- **FreeBSD packaging.** The patches the CloudBSD `graphics/mesa-devel` port
  carries (Track #2309) belong here too.

## The AI policy in Mesa's AGENTS.md, and how this fork treats it

Mesa's `AGENTS.md` (now `UPSTREAM-AGENTS.md`) says that AI agents must not
contribute to the Mesa project, must label AI-written commits, and must not
change some directories, `src/nouveau` among them. Mesa is MIT-licensed.

Mark LaPointe (CloudBSD) made two decisions on 2026-11-22 (Track #2379):

- **Our own fork.** The upstream policy governs contributions to the Mesa
  project, not this private fork, which is never submitted.
- **Our own agent instructions** ("highjack agents.md for every agent"). In
  this fork our `AGENTS.md` governs every agent, so that agents given work
  here, such as opencode rounds, follow CloudBSD's rules and not the upstream
  file. Before this, the upstream file made the opencode model decline every
  `src/nouveau` change.

What agents may do here, and the rules they follow, are stated once, in
`AGENTS.md`. If any patch is ever to leave this fork, `UPSTREAM-AGENTS.md`
applies to it.

## The nouveau kernel-driver code is removed

Mark LaPointe decided on 2026-11-22 (Track #2379): delete everything that
talks to the nouveau kernel driver, and keep NVK, NAK and the nvrm backend.

Deleted from the tree:
- the nvkmd nouveau backend, the nouveau winsys (`src/nouveau/winsys`), the
  nvif headers (`src/nouveau/drm`), the nouveau drm-shim, the MME method
  dumper, `src/util/drm_is_nouveau.h` and `include/drm-uapi/nouveau_drm.h`;
- the NAK hardware runner and tests, and the MME hardware tests. These ran
  on a GPU through that winsys. Their build entries and module lines were
  removed in commit 4b21c1c37f2.

Removed from the build, but kept in the tree on purpose: the gallium nouveau
GL driver and its winsys (`src/gallium/drivers/nouveau`,
`src/gallium/winsys/nouveau`). Nothing builds them since commit
4b21c1c37f2. Keeping the files unbuilt keeps upstream rebases cheap, because
deleting them would turn every upstream change to them into a modify/delete
conflict. The check "no gallium nouveau source in the build" in
`cloudbsd/make.sh` (run by `make test`) fails if any file under those two
directories is compiled, or appears as a target in the build graph.

Kept: NVK, NAK, nil, MME, cubin and the hardware headers in
`src/nouveau/headers`.

The guard is the check "no nouveau uAPI in the build" in `cloudbsd/make.sh`,
run by `make test`. It fails when `include/drm-uapi/nouveau_drm.h` exists,
when any object was compiled with a nouveau uAPI, nvif or libdrm-nouveau
header, or when the build graph has a nouveau winsys target. When it fails,
find what pulled the header or target back in with `ninja -t deps` or
`ninja -t targets all`, and remove that dependency.

What is lost for now: the hardware tests were the only check against real
hardware for the NAK constant folder, the MME macro simulators and the other
NAK hardware tests. A hardware runner on libnvrm can replace them once
libnvrm has channels (S3, #2437). Until then, only the simulation and
builder tests run.

## Building

`make help` lists the targets. `make build` builds NVK with the `nvrm`
backend against a libnvrm tree. `make test` runs the device probe and
`vulkaninfo --summary` on libnvrm's fake backend, so no GPU is needed. See
`cloudbsd/README.md`.

## Status of the nvrm backend

**Stub.** It enumerates GPUs through libnvrm and reports their Vulkan
properties. `vkCreateDevice` returns `VK_ERROR_FEATURE_NOT_PRESENT` until
libnvrm has memory (S2, #2419), channels (S3, #2437) and sync (S4, #2438).
