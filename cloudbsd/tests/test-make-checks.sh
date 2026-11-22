#!/bin/sh
#-
# SPDX-License-Identifier: MIT
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# cloudbsd/tests/test-make-checks.sh: exercise make.sh check command exit codes.
set -eu

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
	printf '%s\n' 'usage: test-make-checks.sh DIRECTORY' >&2
	exit 2
fi

fixtures=$1
top=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$fixtures"

printf '%s\n' 'src/foo.o: include/foo.h' >"$fixtures/clean-deps.txt"
printf '%s\n' 'src/libfoo.a: STATIC_LINKER' >"$fixtures/clean-targets.txt"
printf '%s\n' 'src/foo.o: ../src/gallium/drivers/nouveau/nv50/nv50_screen.c' \
    >"$fixtures/gallium-deps.txt"
printf '%s\n' 'src/gallium/winsys/nouveau/drm/libnouveauwinsys.a: STATIC_LINKER' \
    >"$fixtures/gallium-targets.txt"
: >"$fixtures/empty-targets.txt"
printf '%s\n' 'library_path: libvulkan_nvk_nvrm.so' >"$fixtures/clean.txt"
printf '%s\n' 'library_path: libvulkan_NOUVEAU.so' >"$fixtures/uppercase.txt"
rm -f "$fixtures/missing-deps.txt" "$fixtures/missing.txt"

fail=0
run_case() {
	name=$1
	want=$2
	shift 2
	got=0
	"$@" >/dev/null 2>&1 || got=$?
	if [ "$got" -eq "$want" ]; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s (want %s, got %s)\n' "$name" "$want" "$got"
		fail=1
	fi
}

run_case 'no-gallium-nouveau accepts clean deps and targets' 0 \
    sh "$top/cloudbsd/make.sh" no-gallium-nouveau \
    "$fixtures/clean-deps.txt" "$fixtures/clean-targets.txt"
run_case 'no-gallium-nouveau rejects a deps source' 1 \
    sh "$top/cloudbsd/make.sh" no-gallium-nouveau \
    "$fixtures/gallium-deps.txt" "$fixtures/clean-targets.txt"
run_case 'no-gallium-nouveau rejects a target' 1 \
    sh "$top/cloudbsd/make.sh" no-gallium-nouveau \
    "$fixtures/clean-deps.txt" "$fixtures/gallium-targets.txt"
run_case 'no-gallium-nouveau rejects missing deps' 2 \
    sh "$top/cloudbsd/make.sh" no-gallium-nouveau \
    "$fixtures/missing-deps.txt" "$fixtures/clean-targets.txt"
run_case 'no-gallium-nouveau rejects empty targets' 2 \
    sh "$top/cloudbsd/make.sh" no-gallium-nouveau \
    "$fixtures/clean-deps.txt" "$fixtures/empty-targets.txt"
run_case 'no-nouveau accepts a clean file' 0 \
    sh "$top/cloudbsd/make.sh" no-nouveau "$fixtures/clean.txt"
run_case 'no-nouveau rejects NOUVEAU case-insensitively' 1 \
    sh "$top/cloudbsd/make.sh" no-nouveau "$fixtures/uppercase.txt"
run_case 'no-nouveau rejects a missing file' 2 \
    sh "$top/cloudbsd/make.sh" no-nouveau "$fixtures/missing.txt"
run_case 'no-nouveau rejects no argument' 2 \
    sh "$top/cloudbsd/make.sh" no-nouveau

if [ "$fail" -eq 0 ]; then
	printf '%s\n' 'test-make-checks: ok'
else
	printf '%s\n' 'test-make-checks: FAILED'
	exit 1
fi
