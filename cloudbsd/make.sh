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

die() {
	printf 'make.sh: %s\n' "$*" >&2
	exit 1
}

# The value of a setting, with the characters that are not printable shown
# as a question mark: a value is data and never shell source.
show_value() {
	printf '%s' "$1" | tr -c '[:print:]' '?'
}

nvrm_src=${MK_NVRM_SRC:-$HOME/work/gb10/nvidia-gb10-freebsd}
work=${MK_WORK:-$top/cloudbsd-build}
jobs=${MK_JOBS:-}
# CloudBSD fork: the build needs LLVM 19.  Meson picks llvm-config23 when
# both are installed and then cannot find LLVMSPIRVLib.  The llvm-config to
# use is named in a meson native file; LLVM_CONFIG (or MK_LLVM_CONFIG)
# overrides it.  The value comes from the environment, so it is never read
# as shell source.  The name of the variable that supplied the value is
# kept, so that every message names the control the user has to correct.
#
# A variable that is set is used, even when it is set to nothing: an empty
# LLVM_CONFIG is a mistake of the user and is named as one, and not taken
# for a variable that was never set.
if [ "${LLVM_CONFIG+x}" = x ]; then
	llvm_config=$LLVM_CONFIG
	llvm_config_var=LLVM_CONFIG
elif [ "${MK_LLVM_CONFIG+x}" = x ]; then
	llvm_config=$MK_LLVM_CONFIG
	llvm_config_var=MK_LLVM_CONFIG
else
	llvm_config=/usr/local/bin/llvm-config19
	llvm_config_var=LLVM_CONFIG
fi
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
# CloudBSD fork: one command per line, with a short description, so that no
# command name is split when the terminal is eighty columns wide.
USAGE="usage: make.sh <command>
  preflight           report the tools that are missing
  install-deps        install the packages the build needs (asks first)
  configure           print the build options
  configure-tree      set the Mesa build tree up with the chosen llvm-config
  build               build libnvrm and NVK into WORK/prefix
  test                run the tests on libnvrm's fake backend
  no-nouveau          fail when a file names nouveau
  no-gallium-nouveau  fail when the gallium nouveau sources are built
  clean               remove the build directory"

# Is the version the version of LLVM 19?
#
# Only "19.x" and "19.x.y" are accepted, with the x and the y digits: a
# version that is empty, that is not a version at all ("19junk", "190")
# or that could not be read is refused.
is_llvm19_version() {
	case $1 in
	19.*) ;;
	*) return 1 ;;
	esac
	rest=${1#19.}
	minor=${rest%%.*}
	case $minor in
	''|*[!0-9]*) return 1 ;;
	esac
	case $rest in
	*.*)
		patch=${rest#*.}
		case $patch in
		''|*[!0-9]*) return 1 ;;
		esac
		case $patch in
		*.*) return 1 ;;
		esac
		;;
	esac
	return 0
}

# CloudBSD fork: the llvm-config that was chosen is named once, so that a
# build says which one it uses and where the value came from.
announced=0
announce_llvm_config() {
	[ "$announced" = 0 ] || return 0
	announced=1
	printf '%s\n' "llvm-config: $llvm_config (from $llvm_config_var)"
}

# CloudBSD fork: the llvm-config must be the path of an executable file.
# A value that is not one is refused before anything is built.  Only the
# commands that build Mesa need it: the command that installs LLVM 19,
# the preflight, the usage and the clean must work without it.
#
# Every message says what to do next: the name of the variable that holds
# the value and the value that is expected there.
check_llvm_config() {
	if [ -z "$llvm_config" ]; then
		die "$llvm_config_var is set but empty; unset it or give a path"
	fi
	case $llvm_config in
	/*) ;;
	*) die "$llvm_config_var is not an absolute path: '$(show_value "$llvm_config")'; set $llvm_config_var=/absolute/path/to/llvm-config19" ;;
	esac
	[ -f "$llvm_config" ] ||
		die "$llvm_config_var is not a file: '$(show_value "$llvm_config")'; choose an executable llvm-config and set $llvm_config_var=/absolute/path/to/executable/llvm-config19"
	[ -x "$llvm_config" ] ||
		die "$llvm_config_var is not executable: '$(show_value "$llvm_config")'; choose an executable llvm-config and set $llvm_config_var=/absolute/path/to/executable/llvm-config19"
	if printf '%s' "$llvm_config" | grep -q "['\"]"; then
		die "$llvm_config_var holds a quote: '$(show_value "$llvm_config")'; set $llvm_config_var to a single-line absolute path without quote characters"
	fi
	[ "$(printf '%s' "$llvm_config" | wc -l)" -eq 0 ] ||
		die "$llvm_config_var holds a newline: '$(show_value "$llvm_config")'; set $llvm_config_var to a single-line absolute path without quote characters"
	# CloudBSD fork: the build needs LLVM 19, because LLVMSPIRVLib comes
	# from that installation.  A llvm-config of another version is
	# refused with a message that says what is needed.
	version=$("$llvm_config" --version 2>/dev/null) || version=''
	if ! is_llvm19_version "$version"; then
		die "$llvm_config_var is not LLVM 19: '$(show_value "$version")'; this build needs LLVM 19, so set $llvm_config_var to the llvm-config of an LLVM 19 installation"
	fi
	announce_llvm_config
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
native_file() { printf '%s\n' "$work/native-llvm.ini"; }

# CloudBSD fork: write the meson native file that names the llvm-config the
# build must use.  Meson would otherwise take llvm-config23 and fail to find
# LLVMSPIRVLib.  The file is written at every build, so that the build tree
# always knows which llvm-config it was set up with.
#
# The file is written under a name of its own, made by mktemp inside the
# build directory, and renamed into place: the name of a file that is being
# written must not be predictable, and a reader of the build directory must
# never see half a file.
write_native_file() {
	mkdir -p "$work"
	tmp=$(mktemp "$work/native-llvm.ini.XXXXXX")
	{
		printf '%s\n' '[binaries]'
		printf "llvm-config = '%s'\n" "$llvm_config"
	} >"$tmp"
	mv "$tmp" "$(native_file)"
}

# The native file the build tree was set up with.
native_stamp() { printf '%s\n' "$work/mesa/.native-llvm.stamp"; }

# CloudBSD fork: remember the native file the build tree was set up with.
record_native_file() {
	mkdir -p "$(mesa_dir)"
	cp "$(native_file)" "$(native_stamp)"
}

# Did the native file change since the build tree was set up?
native_file_changed() {
	[ -f "$(native_stamp)" ] || return 0
	! cmp -s "$(native_stamp)" "$(native_file)"
}

# CloudBSD fork: the pkg-config directory of the selected llvm-config comes
# from that llvm-config, so that the package of the build and the llvm-config
# of the build always belong to the same installation.
#
# The llvm-config is asked once, for its prefix and for its libdir, and the
# values it gives are kept.  The candidates that are built from them are
# the ones that are checked and the ones that the message of a failure
# names, so that the check and the message cannot disagree.
#
# The candidates are:
#   - <prefix>/libdata/pkgconfig, the directory of the packages of the
#     LLVM prefix (LLVMSPIRVLib.pc and libclc.pc of the FreeBSD ports);
#   - <libdir>/pkgconfig, the directory that an installation that keeps
#     them next to the libraries uses.
llvm_pkgconfig_query() {
	pkg_prefix=$("$llvm_config" --prefix 2>/dev/null) || pkg_prefix=''
	pkg_libdir=$("$llvm_config" --libdir 2>/dev/null) || pkg_libdir=''
	pkg_candidates=
	pkg_tried=
	if [ -n "$pkg_prefix" ]; then
		pkg_candidates="$pkg_candidates$pkg_prefix/libdata/pkgconfig
"
		pkg_tried="$pkg_tried --prefix => $pkg_prefix/libdata/pkgconfig
"
	else
		pkg_tried="$pkg_tried --prefix => nothing came back
"
	fi
	if [ -n "$pkg_libdir" ]; then
		pkg_candidates="$pkg_candidates$pkg_libdir/pkgconfig
"
		pkg_tried="$pkg_tried --libdir => $pkg_libdir/pkgconfig
"
	else
		pkg_tried="$pkg_tried --libdir => nothing came back
"
	fi
}

# The first candidate that is a directory.  Nothing is printed when no
# candidate is a directory.
llvm_pkgconfig_dir() {
	printf '%s' "$pkg_candidates" | while IFS= read -r dir; do
		[ -n "$dir" ] || continue
		[ -d "$dir" ] || continue
		printf '%s\n' "$dir"
		break
	done
}

# The .pc files the build needs from the selected installation.
#
# They come from the packages of the fork that install into the prefix of
# the LLVM: spirv-llvm-translator-llvm19 and libclc-llvm19.
REQUIRED_PKG_CONFIG="LLVMSPIRVLib libclc"

# CloudBSD fork: the packages the build needs must come from the directory
# of the selected installation.
#
# PKG_CONFIG_PATH is cleared and PKG_CONFIG_LIBDIR names the selected
# directory alone, so that the package of another installation is never
# found.  The file that is found must also be in that directory.
check_required_pkgconfig() {
	dir=$1
	command -v pkg-config >/dev/null 2>&1 ||
		die "pkg-config is not installed; install it (make install-deps) before the build"
	for pkg in $REQUIRED_PKG_CONFIG; do
		if ! PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$dir" \
		    pkg-config --exists "$pkg" 2>/dev/null; then
			die "the pkg-config file $pkg.pc is not in the directory of the $llvm_config_var '$llvm_config' ($dir); install the packages of the fork that provide it (spirv-llvm-translator-llvm19, libclc-llvm19) or select another llvm-config"
		fi
		found=$(PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$dir" \
		    pkg-config --variable=pcfiledir "$pkg" 2>/dev/null) || found=''
		case $found in
		"$dir"|"$dir"/*) ;;
		*) die "the pkg-config file $pkg.pc was found in '$found' and not in the directory of the $llvm_config_var '$llvm_config' ($dir); select another llvm-config" ;;
		esac
	done
}

# The whole PKG_CONFIG_PATH: the directory of the selected llvm-config
# first, then the one of the caller.
#
# When the selected installation has no pkg-config directory, the build
# fails: the directory of the caller may belong to another LLVM, and the
# build would then use two LLVMs at once.
llvm_pkgconfig_path() {
	llvm_pkgconfig_query
	dir=$(llvm_pkgconfig_dir)
	if [ -z "$dir" ]; then
		die "the pkg-config directory of the $llvm_config_var '$llvm_config' was not found; asked $llvm_config for --prefix and --libdir and got: $pkg_tried"
	fi
	check_required_pkgconfig "$dir"
	printf '%s\n' "$dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
}

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
	check_llvm_config
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

# CloudBSD fork: set the build tree up with the native file.  A tree that
# was set up with another llvm-config is reconfigured with the native file,
# so that the setting is never ignored.
configure_mesa() {
	check_llvm_config
	write_native_file
	# CloudBSD fork: LLVMSPIRVLib comes from the pkgconfig directory of
	# the selected llvm-config, and meson resolves the dependencies when it
	# sets the build tree up: the path is in the environment before meson
	# runs.
	pkg_path=$(llvm_pkgconfig_path) || exit 1
	PKG_CONFIG_PATH=$pkg_path
	export PKG_CONFIG_PATH
	if [ ! -f "$(mesa_dir)/build.ninja" ]; then
		mesa_opts_run meson setup "$(mesa_dir)" "$top" \
		    --native-file "$(native_file)"
		record_native_file
		return 0
	fi
	if native_file_changed; then
		mesa_opts_run meson setup --reconfigure "$(mesa_dir)" "$top" \
		    --native-file "$(native_file)"
		record_native_file
	fi
}

build_mesa() {
	configure_mesa
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
	check_llvm_config
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

no_gallium_nouveau() {
	gallium_deps=$1
	gallium_targets=$2
	[ -s "$gallium_deps" ] || {
		printf 'missing or empty ninja deps file: %s\n' "$gallium_deps" >&2
		return 2
	}
	[ -s "$gallium_targets" ] || {
		printf 'missing or empty ninja targets file: %s\n' "$gallium_targets" >&2
		return 2
	}
	gallium_deps_matches=$(grep -E 'src/gallium/drivers/nouveau/|src/gallium/winsys/nouveau/' "$gallium_deps") && gallium_deps_status=0 || gallium_deps_status=$?
	gallium_targets_matches=$(grep -E 'gallium/drivers/nouveau|gallium/winsys/nouveau' "$gallium_targets") && gallium_targets_status=0 || gallium_targets_status=$?
	[ "$gallium_deps_status" -le 1 ] && [ "$gallium_targets_status" -le 1 ] || return 2
	if [ "$gallium_deps_status" -eq 0 ] || [ "$gallium_targets_status" -eq 0 ]; then
		printf 'gallium nouveau in the build:\n' >&2
		[ "$gallium_deps_status" -ne 0 ] || printf '%s\n' "$gallium_deps_matches" >&2
		[ "$gallium_targets_status" -ne 0 ] || printf '%s\n' "$gallium_targets_matches" >&2
		return 1
	fi
	return 0
}

no_gallium_nouveau_returns() {
	want=$1
	shift
	got=0
	no_gallium_nouveau "$@" 2>/dev/null || got=$?
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
	check_llvm_config
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
	printf '%s\n' 'src/foo.o: ../src/gallium/drivers/nouveau/nv50/nv50_screen.c' >"$out/fixture-gallium-nouveau-deps.txt"
	printf '%s\n' 'src/gallium/winsys/nouveau/drm/libnouveauwinsys.a: STATIC_LINKER' >"$out/fixture-gallium-nouveau-targets.txt"
	check "no gallium nouveau source in the build" \
	    no_gallium_nouveau "$out/ninja-deps.txt" "$out/ninja-targets.txt"
	check "gallium nouveau check rejects a planted deps source" \
	    no_gallium_nouveau_returns 1 "$out/fixture-gallium-nouveau-deps.txt" "$out/ninja-targets.txt"
	check "gallium nouveau check rejects a planted target" \
	    no_gallium_nouveau_returns 1 "$out/ninja-deps.txt" "$out/fixture-gallium-nouveau-targets.txt"
	check "make.sh check functions (tests/test-make-checks.sh)" \
	    sh "$top/cloudbsd/tests/test-make-checks.sh" "$out/make-checks"

	# CloudBSD fork: the parser tests of libnvptx, the PTX front end of
	# native CUDA (Track #2426).
	check "libnvptx parser tests" \
	    env TMPDIR="$work/tmp" meson test -C "$(mesa_dir)" \
	    --suite nvptx --print-errorlogs

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
configure-tree) refuse_root; configure_mesa ;;
build) do_build ;;
test) do_test ;;
no-nouveau) no_nouveau "$@" ;;
no-gallium-nouveau) no_gallium_nouveau "$@" ;;
clean) do_clean ;;
*) printf '%s\n' "$USAGE" >&2; exit 1 ;;
esac
