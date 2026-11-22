#!/bin/sh
#-
# SPDX-License-Identifier: MIT
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# cloudbsd/make.sh: the logic behind the fork's Makefile (FORK-NOTES.md).
# Usage: see USAGE below.
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
USAGE="usage: make.sh <preflight|install-deps|configure|build|test|no-nouveau|clean>"

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
icd_json() { printf '%s\n' "$work/prefix/share/vulkan/icd.d/nvk_nvrm_icd.$(arch_name).json"; }
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

mesa_opts_run() {
	set -- "$@" \
	    "--prefix=$work/prefix" \
	    "-Dbuildtype=debugoptimized" \
	    "-Dvulkan-drivers=nvk" \
	    "-Dgallium-drivers=" \
	    "-Dplatforms=" \
	    "-Dglx=disabled" \
	    "-Degl=disabled" \
	    "-Dgbm=disabled" \
	    "-Dopengl=false" \
	    "-Dgles1=disabled" \
	    "-Dgles2=disabled" \
	    "-Dvideo-codecs=" \
	    "-Dvulkan-layers=" \
	    "-Dtools=" \
	    "-Dllvm=enabled" \
	    "-Dnvk-kmd=nvrm" \
	    "-Dnvrm-include-dir=$nvrm_src/lib/libnvrm/include" \
	    "-Dnvrm-lib-dir=$(libnvrm_dir)"
	"$@"
}

do_configure() {
	printf '%s\n' "nothing to configure"
	mesa_opts_run printf '%s\n' "build options:"
	printf '%s\n' "libnvrm from: $nvrm_src/lib/libnvrm" \
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
		mesa_opts_run meson setup "$(mesa_dir)" "$top"
	fi
	mesa_opts_run meson configure "$(mesa_dir)"
	ninja -C "$(mesa_dir)" -j"$(ncpu)"
	meson install -C "$(mesa_dir)" --quiet
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
no_nouveau() {
	if [ "$#" -eq 0 ]; then
		printf '%s\n' 'no_nouveau: no file given (usage: make.sh no-nouveau FILE...)' >&2
		return 2
	fi
	matches=$(grep -il nouveau "$@") && status=0 || status=$?
	case $status in
	0)
		printf 'nouveau found in:\n%s\n' "$matches" >&2
		return 1
		;;
	1) return 0 ;;
	*) return 2 ;;
	esac
}
no_nouveau_returns() {
	want=$1
	shift
	got=0
	no_nouveau "$@" 2>/dev/null || got=$?
	[ "$got" -eq "$want" ]
}

no_nouveau_uapi_deps() {
	[ "$#" -eq 1 ] && [ -s "$1" ] || {
		printf 'missing or empty ninja deps file: %s\n' "${1:-}" >&2
		return 2
	}
	uapi_matches=$(grep -E 'nouveau_drm\.h|drm_is_nouveau\.h|/nvif/|libdrm/nouveau|nouveau_(bo|device|context|private)\.h' "$1") && uapi_status=0 || uapi_status=$?
	case $uapi_status in
	0)
		printf 'nouveau uAPI dependency found:\n%s\n' "$uapi_matches" >&2
		return 1
		;;
	1) return 0 ;;
	*) return 2 ;;
	esac
}

no_nouveau_uapi_deps_returns() {
	want=$1
	file=$2
	got=0
	no_nouveau_uapi_deps "$file" 2>/dev/null || got=$?
	[ "$got" -eq "$want" ]
}

no_nouveau_ws_targets() {
	ws_targets=$1
	ws_ninja_status=$2
	[ "$ws_ninja_status" -eq 0 ] || {
		printf 'ninja -t targets all failed with status %s\n' "$ws_ninja_status" >&2
		return 2
	}
	[ -s "$ws_targets" ] || {
		printf 'missing or empty ninja targets file: %s\n' "$ws_targets" >&2
		return 2
	}
	ws_matches=$(grep -F 'nouveau_ws' "$ws_targets") && ws_grep_status=0 || ws_grep_status=$?
	case $ws_grep_status in
	0)
		printf 'nouveau winsys target found:\n%s\n' "$ws_matches" >&2
		return 1
		;;
	1) return 0 ;;
	*) return 2 ;;
	esac
}

no_nouveau_ws_targets_returns() {
	want=$1
	file=$2
	got=0
	no_nouveau_ws_targets "$file" 0 2>/dev/null || got=$?
	[ "$got" -eq "$want" ]
}

no_nouveau_uapi() {
	uapi_deps=$1
	uapi_ninja_status=$2
	[ "$uapi_ninja_status" -eq 0 ] || {
		printf 'ninja -t deps failed with status %s\n' "$uapi_ninja_status" >&2
		return 2
	}
	[ ! -e "$top/include/drm-uapi/nouveau_drm.h" ] || {
		printf '%s\n' 'include/drm-uapi/nouveau_drm.h still exists' >&2
		return 1
	}
	no_nouveau_uapi_deps "$uapi_deps" || return $?
	if ! grep -qE 'libvulkan_nvk_nvrm|libnvk\.a\.p' "$uapi_deps"; then
		printf 'ninja deps file names no NVK library object (libvulkan_nvk_nvrm or libnvk.a.p): %s\n' "$uapi_deps" >&2
		return 2
	fi
	return 0
}

no_nouveau_uapi_returns() {
	want=$1
	file=$2
	got=0
	no_nouveau_uapi "$file" 0 2>/dev/null || got=$?
	[ "$got" -eq "$want" ]
}

upstream_copies_match() {
	if [ -L "$top/UPSTREAM-AGENTS.md" ] || [ ! -f "$top/UPSTREAM-AGENTS.md" ]; then
		printf '%s\n' 'UPSTREAM-AGENTS.md is missing or not a regular file' >&2
		return 1
	fi
	status=0
	for file in UPSTREAM-CLAUDE.md UPSTREAM-GEMINI.md; do
		if [ -L "$top/$file" ]; then
			printf '%s\n' "$file is a symbolic link; it must be a regular copy" >&2
			status=1
		elif [ ! -f "$top/$file" ]; then
			printf '%s\n' "$file is missing; copy UPSTREAM-AGENTS.md to it" >&2
			status=1
		elif ! cmp -s "$top/$file" "$top/UPSTREAM-AGENTS.md"; then
			printf '%s\n' "$file differs; copy UPSTREAM-AGENTS.md over it" >&2
			status=1
		fi
	done
	return "$status"
}

do_test() {
	refuse_root
	[ -x "$(probe_bin)" ] && [ -f "$(icd_json)" ] || die "no build in $work: run make build first (with the same WORK)"
	mkdir -p "$work/tmp" "$work/test"
	out=$work/test

	installed_names=$(find "$work/prefix" -iname '*nouveau*' -print) && find_status=0 || find_status=$?
	if [ "$find_status" -eq 0 ] && [ -n "$installed_names" ]; then
		printf 'nouveau in installed file names:\n%s\n' "$installed_names" >&2
	fi
	check "no installed file name contains nouveau" \
	    test "$find_status" -eq 0 -a -z "$installed_names"
	check "installed JSON files contain no nouveau names" \
	    find "$work/prefix" -type f -name '*.json' \
	    -exec sh "$top/cloudbsd/make.sh" no-nouveau {} +

	ninja -C "$(mesa_dir)" -t deps >"$out/ninja-deps.txt" && deps_status=0 || deps_status=$?
	check "no nouveau uAPI in the build" \
	    no_nouveau_uapi "$out/ninja-deps.txt" "$deps_status"
	ninja -C "$(mesa_dir)" -t targets all >"$out/ninja-targets.txt" && targets_status=0 || targets_status=$?
	check "no nouveau winsys target in the build graph" \
	    no_nouveau_ws_targets "$out/ninja-targets.txt" "$targets_status"
	printf '%s\n' 'src/foo.o: include/drm-uapi/nouveau_drm.h' >"$out/fixture-nouveau-uapi-deps.txt"
	check "nouveau uAPI deps check rejects a planted header" \
	    no_nouveau_uapi_deps_returns 1 "$out/fixture-nouveau-uapi-deps.txt"
	printf '%s\n' 'src/foo.o: /usr/local/include/libdrm/nouveau/nvif/ioctl.h' >"$out/fixture-nouveau-uapi-nvif-deps.txt"
	check "nouveau uAPI deps check rejects a planted nvif header" \
	    no_nouveau_uapi_deps_returns 1 "$out/fixture-nouveau-uapi-nvif-deps.txt"
	printf '%s\n' 'src/foo.o: include/foo.h' >"$out/fixture-no-nvk-library-deps.txt"
	check "nouveau uAPI check rejects deps with no NVK library object" \
	    no_nouveau_uapi_returns 2 "$out/fixture-no-nvk-library-deps.txt"
	printf '%s\n' 'src/nouveau/winsys/libnouveau_ws.a: STATIC_LINKER' >"$out/fixture-nouveau-winsys-target.txt"
	check "nouveau winsys build-graph check rejects a planted target" \
	    no_nouveau_ws_targets_returns 1 "$out/fixture-nouveau-winsys-target.txt"

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
		expected_driver=$(sed -n 's/.*"expectedDriverName":"\([^"]*\)".*/\1/p' "$out/$p.jsonl" | head -1)
		actual_driver=$(sed -n 's/.*"driverName":"\([^"]*\)".*/\1/p' "$out/$p.jsonl" | head -1)
		check "$p: driver name identifies NVK on libnvrm" \
		    test -n "$expected_driver" -a "$actual_driver" = "$expected_driver"
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
		check "$p: program output contains no nouveau names" \
		    no_nouveau "$out/$p.jsonl" "$out/$p.err" "$out/$p.vulkaninfo"
	done

	printf '%s\n' 'library_path: libvulkan_nouveau.so' >"$out/fixture-leak.txt"
	check "leak check catches a planted nouveau name" \
	    no_nouveau_returns 1 "$out/fixture-leak.txt"
	check "leak check rejects a missing path" \
	    no_nouveau_returns 2 "$out/missing-leak-check"

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

	# UPSTREAM-AGENTS.md is Mesa's AGENTS.md; UPSTREAM-CLAUDE.md and
	# UPSTREAM-GEMINI.md must stay byte-identical copies of it (FORK-NOTES.md).
	# The gate does not scan links, so these must stay regular files.
	check "UPSTREAM-CLAUDE.md and UPSTREAM-GEMINI.md equal UPSTREAM-AGENTS.md" \
	    upstream_copies_match

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
no-nouveau) no_nouveau "$@" ;;
clean) do_clean ;;
*) die "$USAGE" ;;
esac
