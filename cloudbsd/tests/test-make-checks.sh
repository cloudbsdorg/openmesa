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

assert_has() {
	name=$1
	file=$2
	text=$3
	if grep -qF -- "$text" "$file"; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s: %s does not hold %s\n' "$name" "$file" "$text"
		fail=1
	fi
}

assert_lacks() {
	name=$1
	file=$2
	text=$3
	if grep -qF -- "$text" "$file"; then
		printf 'FAIL %s: %s holds %s\n' "$name" "$file" "$text"
		fail=1
	else
		printf 'ok   %s\n' "$name"
	fi
}

assert_count() {
	name=$1
	file=$2
	want=$3
	got=$(grep -c '^meson' "$file" || true)
	if [ "$got" -eq "$want" ]; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s (want %s calls, got %s)\n' "$name" "$want" "$got"
		fail=1
	fi
}

assert_exists() {
	name=$1
	path=$2
	if [ -e "$path" ]; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s: %s is missing\n' "$name" "$path"
		fail=1
	fi
}

assert_absent() {
	name=$1
	path=$2
	if [ -e "$path" ]; then
		printf 'FAIL %s: %s exists\n' "$name" "$path"
		fail=1
	else
		printf 'ok   %s\n' "$name"
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

# CloudBSD fork: the build picks the LLVM that was asked for, and the
# pkg-config directory of that LLVM.  A fake llvm-config and a fake meson
# stand in for the real ones: nothing is built here.

fake_bin=$fixtures/bin
mkdir -p "$fake_bin"

# A fake llvm-config: it names the prefix of the installation it is in.
make_llvm_config() {
	path=$1
	prefix=$2
	cat >"$path" <<EOF
#!/bin/sh
case "\${1:-}" in
--prefix) printf '%s\n' '$prefix' ;;
--libdir) printf '%s\n' '$prefix/lib' ;;
--version) printf '%s\n' '19.1.7' ;;
*) exit 1 ;;
esac
EOF
	chmod +x "$path"
}

# A fake meson: it writes down how it was called and what the environment
# held, and it leaves a build tree behind.
cat >"$fake_bin/meson" <<'EOF'
#!/bin/sh
{
	printf 'meson'
	for arg in "$@"; do
		printf ' %s' "$arg"
	done
	printf '\n'
	printf 'PKG_CONFIG_PATH=%s\n' "${PKG_CONFIG_PATH:-}"
} >>"${MK_MESON_LOG:?}"
if [ -n "${MK_WORK:-}" ]; then
	mkdir -p "$MK_WORK/mesa"
	: >"$MK_WORK/mesa/build.ninja"
fi
exit 0
EOF
chmod +x "$fake_bin/meson"

prefix_a=$fixtures/llvm-a
prefix_b=$fixtures/llvm-b
mkdir -p "$prefix_a/libdata/pkgconfig" "$prefix_b/libdata/pkgconfig"
make_llvm_config "$fixtures/llvm-config-a" "$prefix_a"
make_llvm_config "$fixtures/llvm-config-b" "$prefix_b"

# A llvm-config of another LLVM: the build needs LLVM 19.
cat >"$fixtures/llvm-config-c" <<'EOF'
#!/bin/sh
case "${1:-}" in
--prefix) printf '%s\n' '/nonexistent/llvm-c' ;;
--libdir) printf '%s\n' '/nonexistent/llvm-c/lib' ;;
--version) printf '%s\n' '23.1.0' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-c"

work=$fixtures/work
log=$fixtures/meson.log
rm -rf "$work"
mkdir -p "$work"

configure_tree() {
	llvm=$1
	: >"$log"
	env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
	    LLVM_CONFIG="$llvm" sh "$top/cloudbsd/make.sh" configure-tree || true
}

configure_tree "$fixtures/llvm-config-a"
assert_count 'the first build sets the tree up once' "$log" 1
assert_has 'the pkg-config directory of the llvm-config is in the environment of meson' \
    "$log" "PKG_CONFIG_PATH=$prefix_a/libdata/pkgconfig"
assert_has 'the native file names the llvm-config' "$work/native-llvm.ini" \
    "llvm-config = '$fixtures/llvm-config-a'"

configure_tree "$fixtures/llvm-config-a"
assert_count 'a tree that was set up with the same llvm-config is left alone' \
    "$log" 0

configure_tree "$fixtures/llvm-config-b"
assert_count 'a tree that was set up with another llvm-config is set up again' \
    "$log" 1
assert_has 'the tree is reconfigured with the native file' "$log" '--reconfigure'
assert_has 'the native file names the new llvm-config' \
    "$work/native-llvm.ini" "llvm-config = '$fixtures/llvm-config-b'"

# A llvm-config that is not the path of an executable file is refused.
run_case 'make.sh refuses a llvm-config that is not a path' 1 \
    env LLVM_CONFIG=llvm-config sh "$top/cloudbsd/make.sh" configure-tree
run_case 'make.sh refuses a llvm-config that is not a file' 1 \
    env LLVM_CONFIG=/nonexistent/llvm-config \
    sh "$top/cloudbsd/make.sh" configure-tree
run_case 'make.sh refuses a llvm-config that is not LLVM 19' 1 \
    env LLVM_CONFIG="$fixtures/llvm-config-c" \
    sh "$top/cloudbsd/make.sh" configure-tree

# The value of LLVM_CONFIG is data and never shell source: a value that
# holds a shell word must not run anything.
sentinel=$fixtures/injected
rm -f "$sentinel"
injection='"; touch '"$sentinel"'; #'
out=$fixtures/injection.out
mkdir -p "$fixtures/no-libnvrm"
(cd "$top" && make build NVRM_SRC="$fixtures/no-libnvrm" WORK="$work" \
    LLVM_CONFIG="$injection") >"$out" 2>&1 || true
assert_absent 'a LLVM_CONFIG that holds a shell word runs nothing' "$sentinel"
assert_has 'make.sh refuses a LLVM_CONFIG that holds a shell word' "$out" \
    'LLVM_CONFIG is not an absolute path'
assert_has 'make.sh names the value it refused' "$out" "$injection"

if [ "$fail" -eq 0 ]; then
	printf '%s\n' 'test-make-checks: ok'
else
	printf '%s\n' 'test-make-checks: FAILED'
	exit 1
fi
