# SPDX-License-Identifier: MIT
# Copyright (c) 2026 REVYTECH, Inc.
#
# CloudBSD private fork of Mesa (FORK-NOTES.md).  A thin wrapper around
# Mesa's own meson build (skill project-makefile-contract, purpose "fork"):
# it builds NVK with the nvkmd "nvrm" backend against a libnvrm tree and
# runs the hardware-free tests on libnvrm's fake backend.  Every recipe hands
# its target to cloudbsd/make.sh, which holds the logic in sh(1).
#
# Portable for GNU make and bmake: recipe lines start with a tab, no make
# conditionals, no GNU functions, no bmake modifiers, no include.

REPO_PURPOSE = fork
# Upstream builds with meson; this wrapper only chooses the options.
UPSTREAM_BUILD = meson setup <builddir> && ninja -C <builddir>
# nvidia-gb10-freebsd checkout that holds lib/libnvrm; empty means
# $HOME/work/gb10/nvidia-gb10-freebsd.
NVRM_SRC =
# open-gpu-kernel-modules headers for libnvrm; empty lets libnvrm pick.
OGKM =
# Where the build goes; empty means ./cloudbsd-build in this checkout.
WORK =
JOBS =
YES = 0
ASSUME_YES = 0
JSON = 0

DRIVER = sh cloudbsd/make.sh
DRIVER_ENV = MK_NVRM_SRC="$(NVRM_SRC)" MK_OGKM="$(OGKM)" MK_WORK="$(WORK)" MK_JOBS="$(JOBS)" MK_YES="$(YES)" MK_ASSUME_YES="$(ASSUME_YES)" MK_JSON="$(JSON)" MK_PURPOSE="$(REPO_PURPOSE)"

help:
	@printf '%s\n' \
		"purpose: $(REPO_PURPOSE)" \
		"CloudBSD private fork of Mesa: NVK on NVIDIA's open RM through libnvrm" \
		"  (see FORK-NOTES.md)." \
		"Targets (same names on every OS):" \
		"  make purpose       print REPO_PURPOSE" \
		"  make help          this message (default)" \
		"  make preflight     report missing tools (JSON=1 for --json)" \
		"  make install-deps  install missing packages with pkg" \
		"                     (asks; YES=1 to skip asking)" \
		"  make configure     nothing to configure (prints the build options)" \
		"  make build         build libnvrm, then NVK into WORK/prefix" \
		"                     (see make configure for the options)" \
		"  make test          vulkaninfo --summary and the device probe" \
		"                     on libnvrm's fake backend" \
		"  make clean         remove the build directory" \
		"" \
		"Switches:" \
		"  NVRM_SRC=dir    nvidia-gb10-freebsd checkout" \
		"                  (default \$$HOME/work/gb10/nvidia-gb10-freebsd)" \
		"  WORK=dir        build directory (default ./cloudbsd-build)" \
		"  JOBS=n          parallel jobs (default: half of hw.ncpu)" \
		"  YES=1           do it without asking (install-deps)" \
		"  JSON=1          preflight writes the --json contract" \
		"" \
		"PATH, PYTHONPATH, PKG_CONFIG_PATH and LLVM_CONFIG from the environment" \
		"are used as they are." \
		"Upstream build: $(UPSTREAM_BUILD)"

.PHONY: help purpose preflight deps install-deps configure build test clean

purpose:
	@printf '%s\n' "$(REPO_PURPOSE)"

preflight:
	@if [ "$(JSON)" = 1 ]; then \
		$(DRIVER_ENV) $(DRIVER) preflight --json --purpose "$(REPO_PURPOSE)"; \
	else \
		$(DRIVER_ENV) $(DRIVER) preflight --purpose "$(REPO_PURPOSE)"; \
	fi

deps install-deps:
	@$(DRIVER_ENV) $(DRIVER) install-deps

configure:
	@$(DRIVER_ENV) $(DRIVER) configure

build:
	@$(DRIVER_ENV) $(DRIVER) build

test:
	@$(DRIVER_ENV) $(DRIVER) test

clean:
	@$(DRIVER_ENV) $(DRIVER) clean
