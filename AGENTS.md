# AGENTS.md (CloudBSD fork of Mesa)

These are the instructions for every agent working in **CloudBSD's private
fork of Mesa**, `cloudbsdorg/mesa-cloudbsd`. `CLAUDE.md` and `GEMINI.md` link
to this file. It replaces Mesa's own `AGENTS.md` in this fork, by decision of
Mark LaPointe on 2026-11-22 (Track #2379; the reasons are in
`FORK-NOTES.md`).

`UPSTREAM-AGENTS.md`, `UPSTREAM-CLAUDE.md` and `UPSTREAM-GEMINI.md` hold Mesa's
own text, kept for reference. **They are not instructions for work in this
fork.** They apply only to a patch that is to leave the fork (below).

## What this tree is

- A private fork of Mesa (MIT-licensed). It is never submitted upstream: no
  merge request, issue or comment goes to the Mesa GitLab from this work.
- If a patch is ever to leave this fork, `UPSTREAM-AGENTS.md` applies to it,
  and a person decides about it, rewrites it and submits it under Mesa's
  rules. That is a separate decision.
- NVK here runs on **libnvrm**, CloudBSD's library for NVIDIA's open RM,
  and our NVIDIA driver (`nvidia.ko`) on FreeBSD. The backend is the nvkmd
  `nvrm` backend in `src/nouveau/vulkan/nvkmd/nvrm/`. **Nothing here uses the
  nouveau kernel driver.** The directory name `src/nouveau` is Mesa's, kept so
  that upstream merges stay cheap.

## What agents may change

Any directory of this tree, including `src/nouveau` (NVK, NAK, nil and the
hardware headers). The work is CloudBSD's: the nvkmd nvrm backend, the NAK
compiler work for native CUDA, and the FreeBSD build fixes from the
`graphics/mesa-devel` port.

## Rules

- **Commit gate:** stage the change, then run `review-and-record`. It runs the
  scanners and the review voices over the exact staged diff and writes the
  receipt the commit hook needs. Fix what it finds, or say in the commit
  message why a finding is deferred. There are two review rounds per
  intended commit. Never use `--no-verify`.
- **Commits:** the author is Mark LaPointe <mark@cloudbsd.org>. The subject
  names the Track work package (`#NNNN`). The body ends with the stewardship
  block the commit hook checks, with these lines:
  `Stewardship:`, `- Honcho: ...`, `- Ticket: #NNNN`,
  `- Ticket updated: ...` and `- Docs: ...`.
- **No tool trailers:** no `Generated-by`, `Assisted-by`, `Co-Authored-By`
  or session lines.
- **Docs with every change:** a user-visible interface gets its README text
  and its `--help`. A tool also gets a man page (`mandoc -T lint` clean).
- **JSON contracts:** tools print `--json` (and `--jsonl` for streams) with a
  versioned schema.
- **What may be read:** never read Linux kernel source, other GPL source, or
  the source of the nouveau kernel driver. Mesa itself is MIT and may be read,
  `src/nouveau` included. NVIDIA's open-gpu-kernel-modules headers are MIT
  and may be read.
- **Names users see:** installed files, the ICD manifest, driver strings and
  reports do not say "nouveau". The nvrm build is NVK on libnvrm.
- **Mesa code style:** code under `src/` follows Mesa's style (the
  surrounding file and `docs/codingstyle.rst`), so that upstream merges stay
  clean. Mark every fork change in an upstream file with a short
  `CloudBSD fork:` comment. CloudBSD's own files (`cloudbsd/`, `Makefile`)
  follow CloudBSD's style.
- No root and no kernel module loading for builds or tests. `make test` runs
  on the libnvrm fake backend.

## Where things are

- `FORK-NOTES.md`: branches, why we fork, and the policy decisions.
- `cloudbsd/README.md`: build, test and the device probe.
- The design: `mesa/nvk-nvrm/DESIGN.md` in the private repository
  `cloudbsdorg/nvidia-gb10-freebsd` (Track #2379).
