#!/bin/sh
#-
# SPDX-License-Identifier: MIT
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# cloudbsd/make.sh: the logic behind the fork's Makefile (FORK-NOTES.md).
# Usage: sh cloudbsd/make.sh <preflight|install-deps|configure|build|test|clean>
# Settings come from MK_* in the environment (the Makefile passes them).
set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
cmd=${1:-}
[ $# -gt 0 ] && shift

nvrm_src=${MK_NVRM_SRC:-$HOME/work/gb10/nvidia-gb10-freebsd}
work=${MK_WORK:-$top/cloudbsd-build}
jobs=${MK_JOBS:-}
yes=0
if [ "${MK_YES:-0}" = 1 ] || [ "${MK_ASSUME_YES:-0}" = 1 ]; then
	yes=1
fi

# The packages a FreeBSD build needs (pkg names; FreeBSD 16-CURRENT ports).
PKGS="meson ninja pkgconf python312 py312-mako py312-pyyaml py312-packaging \
rust rust-bindgen-cli llvm19 libclc-llvm19 spirv-llvm-translator-llvm19 \
spirv-tools glslang libdrm expat bison vulkan-headers vulkan-loader \
vulkan-tools"
TOOLS="git cc meson ninja rustc bindgen python3 pkg-config vulkaninfo"

die() {
	printf 'make.sh: %s\n' "$*" >&2
	exit 1
}

# Builds and tests run as a normal user: the Python and meson scripts of the
# tree never run as root, and no root-owned files land in the build tree.
refuse_root() {
	if [ "$(id -u)" = 0 ]; then
		die "refusing to run '$cmd' as root; run it as a normal user (only install-deps needs root)"
	fi
}

ncpu() {
	if [ -n "$jobs" ]; then
		printf '%s\n' "$jobs"
		return
	fi
	n=$(/sbin/sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
	n=$((n / 2))
	[ "$n" -ge 1 ] || n=1
	printf '%s\n' "$n"
}

arch_name() {
	case $(uname -m) in
	arm64|aarch64) printf '%s\n' aarch64 ;;
	amd64|x86_64) printf '%s\n' x86_64 ;;
	*) uname -m ;;
	esac
}

libnvrm_dir() { printf '%s\n' "$work/libnvrm"; }
mesa_dir() { printf '%s\n' "$work/mesa"; }
icd_json() { printf '%s\n' "$(mesa_dir)/src/nouveau/vulkan/nouveau_devenv_icd.$(arch_name).json"; }
probe_bin() { printf '%s\n' "$work/probe/nvk-nvrm-probe"; }

do_preflight() {
	PREFLIGHT_COMMANDS=$TOOLS sh "$top/cloudbsd/preflight.sh" "$@"
}

do_install_deps() {
	[ "$(uname -s)" = FreeBSD ] || die "install-deps knows FreeBSD pkg only; install: $PKGS"
	printf '%s\n' "install-deps: pkg install $PKGS"
	if [ "$yes" != 1 ]; then
		if [ ! -t 0 ]; then
			die "install-deps: no terminal to ask; run again with YES=1 if you are fully aware"
		fi
		printf '%s' "install these packages? type YES: "
		read -r ans
		[ "$ans" = YES ] || die "install-deps: not confirmed"
	fi
	if [ "$(id -u)" = 0 ]; then
		pkg install -y $PKGS
	else
		sudo pkg install -y $PKGS
	fi
}

do_configure() {
	printf '%s\n' "nothing to configure" \
	    "build options: -Dvulkan-drivers=nouveau -Dnvk-kmd=nvrm -Dgallium-drivers= -Dplatforms=" \
	    "libnvrm from: $nvrm_src/lib/libnvrm" \
	    "build directory: $work"
}

build_libnvrm() {
	src=$nvrm_src/lib/libnvrm
	[ -f "$src/include/nvrm/nvrm.h" ] || die "no libnvrm at $src (set NVRM_SRC)"
	mkdir -p "$(libnvrm_dir)"
	set -- MAKEOBJDIR="$(libnvrm_dir)" MK_PROFILE=no MK_TESTS=no WITHOUT_MAN=yes
	if [ -n "${MK_OGKM:-}" ]; then
		set -- "$@" OGKM="$MK_OGKM"
	fi
	# libnvrm is a FreeBSD bsd.lib.mk library: build it with the base make.
	/usr/bin/make -C "$src" "$@"
	[ -f "$(libnvrm_dir)/libnvrm.so" ] || die "libnvrm did not build"
}

build_mesa() {
	if [ ! -f "$(mesa_dir)/build.ninja" ]; then
		meson setup "$(mesa_dir)" "$top" \
		    -Dbuildtype=debugoptimized \
		    -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms= \
		    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
		    -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs= \
		    -Dvulkan-layers= -Dtools= -Dllvm=enabled \
		    -Dnvk-kmd=nvrm \
		    -Dnvrm-include-dir="$nvrm_src/lib/libnvrm/include" \
		    -Dnvrm-lib-dir="$(libnvrm_dir)"
	fi
	ninja -C "$(mesa_dir)" -j"$(ncpu)" \
	    src/nouveau/vulkan/libvulkan_nouveau.so \
	    "src/nouveau/vulkan/nouveau_devenv_icd.$(arch_name).json"
}

build_probe() {
	mkdir -p "$work/probe"
	${CC:-cc} -std=c11 -Wall -Wextra -Werror -I/usr/local/include \
	    -I"$top/src/nouveau/vulkan" -o "$(probe_bin)" \
	    "$top/cloudbsd/tests/nvk-nvrm-probe.c" -L/usr/local/lib -lvulkan
}

do_build() {
	refuse_root
	build_libnvrm
	build_mesa
	build_probe
	printf '%s\n' "build: ok" "  ICD: $(icd_json)" "  probe: $(probe_bin)"
}

# run_fake <profile> [VAR=value...] <command...>: run with only this ICD and
# the fake backend; extra assignments go to the environment too.
run_fake() {
	profile=$1
	shift
	env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/local/bin \
	    TMPDIR="$work/tmp" NVRM_BACKEND=fake NVRM_FAKE_PROFILE="$profile" \
	    LD_LIBRARY_PATH="$(libnvrm_dir)" VK_ICD_FILENAMES="$(icd_json)" \
	    NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 "$@"
}

fail=0
check() {
	name=$1
	shift
	if "$@"; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s\n' "$name"
		fail=1
	fi
}

has() { grep -qF -- "$2" "$1"; }
lacks() { ! grep -qF -- "$2" "$1"; }

do_test() {
	refuse_root
	[ -x "$(probe_bin)" ] && [ -f "$(icd_json)" ] || die "run make build first"
	mkdir -p "$work/tmp" "$work/test"
	out=$work/test

	# profile:chipset:PCI device id, from libnvrm's fake backend
	# (lib/libnvrm/nvrm_fake.c): discrete is GB206-shaped, unified GB10.
	for row in discrete:NV1B6:11524 unified:NV1BB:11794; do
		p=${row%%:*}
		rest=${row#*:}
		chip=${rest%%:*}
		devid=${rest#*:}

		run_fake "$p" "$(probe_bin)" >"$out/$p.jsonl" 2>"$out/$p.err" || :
		check "$p: probe lists one NVK device" \
		    test "$(grep -c '"driverIsMesaNvk":true' "$out/$p.jsonl")" = 1
		check "$p: device name marks the stub" \
		    has "$out/$p.jsonl" "\"deviceName\":\"NVIDIA libnvrm stub (NVK $chip)\""
		check "$p: PCI device id from libnvrm" \
		    has "$out/$p.jsonl" "\"deviceID\":$devid,"
		check "$p: vkCreateDevice is refused by the stub" \
		    has "$out/$p.jsonl" '"createDevice":"VK_ERROR_FEATURE_NOT_PRESENT"'
		# The expected text comes from the probe, which takes it from the
		# backend's own header: one source, no copy here.
		msg=$(sed -n 's/.*"stubMessage":"\([^"]*\)".*/\1/p' "$out/$p.jsonl" | head -1)
		check "$p: the stub logged its line" \
		    test -n "$msg" -a "$(grep -cF -- "$msg" "$out/$p.err")" -ge 1

		# vulkaninfo creates a device before it prints, so on the stub it
		# must stop at the stub's refusal, not crash or list nothing.
		run_fake "$p" vulkaninfo --summary >"$out/$p.vulkaninfo" 2>&1 || :
		check "$p: vulkaninfo --summary reaches the stub" \
		    has "$out/$p.vulkaninfo" "ERROR_FEATURE_NOT_PRESENT"
		check "$p: vulkaninfo did not crash" \
		    lacks "$out/$p.vulkaninfo" "Segmentation fault"
	done

	# No GPU, and no nvidia driver at all: NVK lists no device and reports no
	# error of its own.  (With no device in any ICD the loader itself answers
	# VK_ERROR_INITIALIZATION_FAILED; that is the loader, not NVK.)
	for f in nogpu open; do
		run_fake discrete NVRM_FAKE_FAIL=$f "$(probe_bin)" \
		    >"$out/$f.jsonl" 2>"$out/$f.err" || :
		check "fake failure $f: no NVK device" \
		    has "$out/$f.jsonl" '"error":"no physical device"'
		check "fake failure $f: no error from NVK" \
		    lacks "$out/$f.err" "MESA: error"
		check "fake failure $f: the loader did not reject the ICD" \
		    lacks "$out/$f.err" "Failed in ICD"
	done

	# Fail-closed VRAM rule (DESIGN.md note 6.2): the discrete profile has no
	# write-combined BAR signal, so no memory type may be device-local and
	# host-visible (flags 1|2 = 3) on the VRAM heap.
	check "discrete: no host-visible VRAM memory type" \
	    lacks "$out/discrete.jsonl" '{"heap":0,"flags":7}'

	if [ "$fail" = 0 ]; then
		printf '%s\n' "test: ok"
	else
		printf '%s\n' "test: FAILED (logs in $out)"
		exit 1
	fi
}

do_clean() {
	refuse_root
	[ -n "$work" ] || die "empty build directory"
	case $work in
	/|"$HOME"|"$top") die "refusing to remove $work" ;;
	esac
	rm -rf "${work:?}"
	printf '%s\n' "clean: removed $work"
}

case $cmd in
preflight) do_preflight "$@" ;;
install-deps) do_install_deps ;;
configure) do_configure ;;
build) do_build ;;
test) do_test ;;
clean) do_clean ;;
*) die "usage: make.sh <preflight|install-deps|configure|build|test|clean>" ;;
esac
