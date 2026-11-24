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

assert_no_long_line() {
	name=$1
	file=$2
	width=$3
	long=$(awk -v w="$width" 'length > w { print; exit }' "$file")
	if [ -z "$long" ]; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s: %s holds a line of more than %s columns: %s\n' \
		    "$name" "$file" "$width" "$long"
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

# The .pc files that the build needs from the selected installation.  The
# build looks for them in the directory of the chosen llvm-config alone, so
# that the package of another installation is never used.
make_pkgconfig() {
	dir=$1
	mkdir -p "$dir"
	for pkg in LLVMSPIRVLib libclc; do
		cat >"$dir/$pkg.pc" <<EOF
prefix=$dir
libdir=$dir
includedir=$dir

Name: $pkg
Description: the $pkg of the test
Version: 19.1.7
Libs: -L$dir -l$pkg
Cflags: -I$dir
EOF
	done
}

prefix_a=$fixtures/llvm-a
prefix_b=$fixtures/llvm-b
mkdir -p "$prefix_a/libdata/pkgconfig" "$prefix_b/libdata/pkgconfig"
make_pkgconfig "$prefix_a/libdata/pkgconfig"
make_pkgconfig "$prefix_b/libdata/pkgconfig"
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

# The native file is written under a name of its own and renamed into place:
# no file with a predictable name is left behind.
leftovers=$(find "$work" -maxdepth 1 -name 'native-llvm.ini.*' 2>/dev/null | head -1)
if [ -z "$leftovers" ]; then
	printf 'ok   %s\n' 'the native file leaves no temporary file behind'
else
	printf 'FAIL %s: %s holds %s\n' \
	    'the native file leaves no temporary file behind' "$work" "$leftovers"
	fail=1
fi

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

# CloudBSD fork: the commands that do not use LLVM must run without it.
# The target that installs LLVM 19 cannot be blocked by the check of
# LLVM 19, and the user must be able to read the usage and to clean the
# build directory on a host that has no LLVM at all.

missing_llvm=/nonexistent/llvm-config

out=$fixtures/no-llvm-usage.out
env LLVM_CONFIG="$missing_llvm" sh "$top/cloudbsd/make.sh" >"$out" 2>&1 || true
assert_has 'the usage does not need an llvm-config' "$out" 'usage: make.sh'
assert_lacks 'the usage does not check the llvm-config' "$out" 'LLVM_CONFIG'

out=$fixtures/no-llvm-install-deps.out
env LLVM_CONFIG="$missing_llvm" sh "$top/cloudbsd/make.sh" install-deps \
    </dev/null >"$out" 2>&1 || true
assert_has 'install-deps does not need an llvm-config' "$out" \
    'install-deps: pkg install'
assert_lacks 'install-deps does not check the llvm-config' "$out" 'LLVM_CONFIG'

out=$fixtures/no-llvm-preflight.out
env LLVM_CONFIG="$missing_llvm" sh "$top/cloudbsd/make.sh" preflight \
    </dev/null >"$out" 2>&1 || true
assert_has 'preflight does not need an llvm-config' "$out" 'preflight:'
assert_lacks 'preflight does not check the llvm-config' "$out" 'LLVM_CONFIG'

clean_work=$fixtures/clean-work
rm -rf "$clean_work"
mkdir -p "$clean_work"
out=$fixtures/no-llvm-clean.out
env LLVM_CONFIG="$missing_llvm" MK_WORK="$clean_work" \
    sh "$top/cloudbsd/make.sh" clean >"$out" 2>&1 || true
assert_has 'clean does not need an llvm-config' "$out" "clean: removed $clean_work"
assert_lacks 'clean does not check the llvm-config' "$out" 'LLVM_CONFIG'

# The commands that build Mesa still need the LLVM they were told to use.
run_case 'configure-tree refuses a missing llvm-config' 1 \
    env LLVM_CONFIG="$missing_llvm" sh "$top/cloudbsd/make.sh" configure-tree
run_case 'build refuses a missing llvm-config' 1 \
    env LLVM_CONFIG="$missing_llvm" MK_WORK="$work" \
    sh "$top/cloudbsd/make.sh" build

# CloudBSD fork: the pkg-config directory of the selected llvm-config must
# exist.  When it does not, the build fails instead of using the
# pkg-config path of the caller, which may belong to another LLVM.
mkdir -p "$fixtures/llvm-nopkg"
cat >"$fixtures/llvm-config-nopkg" <<EOF
#!/bin/sh
case "\${1:-}" in
--prefix) printf '%s\n' '$fixtures/llvm-nopkg' ;;
--libdir) printf '%s\n' '$fixtures/llvm-nopkg/lib' ;;
--version) printf '%s\n' '19.1.7' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-nopkg"

: >"$log"
out=$fixtures/no-pkgconfig.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG="$fixtures/llvm-config-nopkg" \
    PKG_CONFIG_PATH="$prefix_b/libdata/pkgconfig" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_count 'no meson runs without a pkg-config directory' "$log" 0
assert_has 'the refusal names the llvm-config' "$out" \
    "$fixtures/llvm-config-nopkg"
assert_has 'the refusal names the directory that was tried' "$out" \
    "$fixtures/llvm-nopkg/libdata/pkgconfig"

# CloudBSD fork: a directory that exists but holds none of the packages
# that the build needs is not enough.  The package of the caller must not
# be used: the build would then mix two LLVM installations.
mkdir -p "$fixtures/llvm-emptypkg/libdata/pkgconfig"
cat >"$fixtures/llvm-config-emptypkg" <<EOF
#!/bin/sh
case "\${1:-}" in
--prefix) printf '%s\n' '$fixtures/llvm-emptypkg' ;;
--libdir) printf '%s\n' '$fixtures/llvm-emptypkg/lib' ;;
--version) printf '%s\n' '19.1.7' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-emptypkg"

mkdir -p "$fixtures/caller-pkgconfig"
cp "$prefix_a/libdata/pkgconfig/LLVMSPIRVLib.pc" \
    "$fixtures/caller-pkgconfig/LLVMSPIRVLib.pc"

: >"$log"
out=$fixtures/empty-pkgconfig.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG="$fixtures/llvm-config-emptypkg" \
    PKG_CONFIG_PATH="$fixtures/caller-pkgconfig" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_count 'no meson runs when the selected directory holds no package' \
    "$log" 0
assert_has 'the refusal names the package that is missing' "$out" \
    'LLVMSPIRVLib.pc'
assert_has 'the refusal names the directory of the selected llvm-config' \
    "$out" "$fixtures/llvm-emptypkg/libdata/pkgconfig"

# CloudBSD fork: the llvm-config is asked once for its installation.  A
# llvm-config that answers with another prefix the second time it is asked
# must not make the check and the message of the failure disagree: the
# message names the candidate that was checked.
cat >"$fixtures/llvm-config-switching" <<EOF
#!/bin/sh
case "\${1:-}" in
--prefix)
	counter="$work/switching.prefix.count"
	n=\$(cat "\$counter" 2>/dev/null || echo 0)
	n=\$((n + 1))
	printf '%s\n' "\$n" >"\$counter"
	if [ "\$n" -le 1 ]; then
		printf '%s\n' '$fixtures/llvm-switch-a'
	else
		printf '%s\n' '$fixtures/llvm-switch-b'
	fi
	;;
--libdir) printf '%s\n' '$fixtures/llvm-switch-b/lib' ;;
--version) printf '%s\n' '19.1.7' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-switching"

: >"$log"
out=$fixtures/switching.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG="$fixtures/llvm-config-switching" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_count 'no meson runs without a pkg-config directory' "$log" 0
assert_has 'the refusal names the candidate that was checked' "$out" \
    "$fixtures/llvm-switch-a/libdata/pkgconfig"
assert_lacks 'the refusal does not name another candidate' "$out" \
    "$fixtures/llvm-switch-b/libdata/pkgconfig"

# CloudBSD fork: a llvm-config that cannot say where its installation is
# is named as such, and the list of what was tried is never empty.
cat >"$fixtures/llvm-config-noprefix" <<'EOF'
#!/bin/sh
case "${1:-}" in
--version) printf '%s\n' '19.1.7' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-noprefix"

out=$fixtures/noprefix.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG="$fixtures/llvm-config-noprefix" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_has 'the refusal says what was asked and what came back' "$out" \
    '--prefix => nothing came back'
assert_has 'the refusal says what was asked and what came back' "$out" \
    '--libdir => nothing came back'

# CloudBSD fork: a variable that is set but empty is a mistake of the
# user, and it is named as one.  It must not fall back to the other
# variable or to the default.
out=$fixtures/empty-llvm-config.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG='' MK_LLVM_CONFIG="$fixtures/llvm-config-a" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_has 'an empty LLVM_CONFIG is refused' "$out" \
    'LLVM_CONFIG is set but empty'
assert_lacks 'an empty LLVM_CONFIG does not fall back to MK_LLVM_CONFIG' \
    "$out" 'MK_LLVM_CONFIG'
run_case 'make.sh refuses an empty LLVM_CONFIG' 1 \
    env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG='' MK_LLVM_CONFIG="$fixtures/llvm-config-a" \
    sh "$top/cloudbsd/make.sh" configure-tree

# CloudBSD fork: every error of the llvm-config says what to do next.
: >"$fixtures/llvm-config-not-executable"
out=$fixtures/not-executable.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG="$fixtures/llvm-config-not-executable" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_has 'the refusal says what to do about a file that is not executable' \
    "$out" 'choose an executable llvm-config'

cp "$fixtures/llvm-config-a" "$fixtures/llvm-config\"quoted"
chmod +x "$fixtures/llvm-config\"quoted"
out=$fixtures/quoted.out
env PATH="$fake_bin:$PATH" MK_WORK="$work" MK_MESON_LOG="$log" \
    LLVM_CONFIG="$fixtures/llvm-config\"quoted" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_has 'the refusal says what to do about a path that holds a quote' \
    "$out" 'without quote characters'

# CloudBSD fork: only a version of LLVM 19 is accepted.  An llvm-config
# that fails, that says nothing, or that says something that is not a
# version, is refused.
cat >"$fixtures/llvm-config-failversion" <<'EOF'
#!/bin/sh
case "${1:-}" in
--prefix) printf '%s\n' '/nonexistent/llvm-failversion' ;;
--libdir) printf '%s\n' '/nonexistent/llvm-failversion/lib' ;;
--version) exit 1 ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-failversion"

cat >"$fixtures/llvm-config-emptyversion" <<'EOF'
#!/bin/sh
case "${1:-}" in
--prefix) printf '%s\n' '/nonexistent/llvm-emptyversion' ;;
--libdir) printf '%s\n' '/nonexistent/llvm-emptyversion/lib' ;;
--version) printf '%s' '' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-emptyversion"

cat >"$fixtures/llvm-config-junkversion" <<'EOF'
#!/bin/sh
case "${1:-}" in
--prefix) printf '%s\n' '/nonexistent/llvm-junkversion' ;;
--libdir) printf '%s\n' '/nonexistent/llvm-junkversion/lib' ;;
--version) printf '%s\n' '19junk' ;;
*) exit 1 ;;
esac
EOF
chmod +x "$fixtures/llvm-config-junkversion"

run_case 'make.sh refuses a llvm-config whose version fails' 1 \
    env LLVM_CONFIG="$fixtures/llvm-config-failversion" \
    sh "$top/cloudbsd/make.sh" configure-tree
run_case 'make.sh refuses a llvm-config whose version is empty' 1 \
    env LLVM_CONFIG="$fixtures/llvm-config-emptyversion" \
    sh "$top/cloudbsd/make.sh" configure-tree
run_case 'make.sh refuses a llvm-config that reports 19junk' 1 \
    env LLVM_CONFIG="$fixtures/llvm-config-junkversion" \
    sh "$top/cloudbsd/make.sh" configure-tree

out=$fixtures/junkversion.out
env LLVM_CONFIG="$fixtures/llvm-config-junkversion" \
    sh "$top/cloudbsd/make.sh" configure-tree >"$out" 2>&1 || true
assert_has 'the refusal names the version it refused' "$out" '19junk'

# CloudBSD fork: the value of LLVM_CONFIG is data.  Make must not expand
# it as make syntax, and the value must reach make.sh as it was written.
sentinel=$fixtures/make-expansion-sentinel
for mk in make gmake; do
	command -v "$mk" >/dev/null 2>&1 || continue

	# The value in the environment: it reaches make.sh as it was written.
	rm -f "$sentinel"
	out=$fixtures/make-expansion-$mk.out
	(cd "$top" && LLVM_CONFIG='$$(shell touch '"$sentinel"')' \
	    "$mk" build NVRM_SRC="$fixtures/no-libnvrm" WORK="$work") \
	    >"$out" 2>&1 || true
	assert_absent "the value of LLVM_CONFIG is not run by $mk" "$sentinel"
	assert_has "the value of LLVM_CONFIG reaches make.sh as it was written by $mk" \
	    "$out" '$$(shell touch'

	# The value as a variable of make: make expands it by itself, but it
	# is never run.
	rm -f "$sentinel"
	out=$fixtures/make-expansion-variable-$mk.out
	(cd "$top" && "$mk" build NVRM_SRC="$fixtures/no-libnvrm" WORK="$work" \
	    LLVM_CONFIG='$$(shell touch '"$sentinel"')') >"$out" 2>&1 || true
	assert_absent "the value of LLVM_CONFIG is not run as make syntax by $mk" \
	    "$sentinel"

	# The value as a variable of make reaches make.sh: make.sh names the
	# llvm-config it chose, and the name is the path that was given.
	cmdline_llvm=$fixtures/llvm-config-cmdline-$mk
	make_llvm_config "$cmdline_llvm" "$fixtures/llvm-cmdline-$mk"
	out=$fixtures/make-command-line-$mk.out
	(cd "$top" && "$mk" build NVRM_SRC="$fixtures/no-libnvrm" WORK="$work" \
	    LLVM_CONFIG="$cmdline_llvm") >"$out" 2>&1 || true
	assert_has "the value of LLVM_CONFIG on the command line reaches make.sh by $mk" \
	    "$out" "$cmdline_llvm"
done

# CloudBSD fork: the diagnostic names the variable the user set, and the
# usage names every command on a line of its own.
out=$fixtures/mk-llvm-config-name.out
env MK_LLVM_CONFIG=relative sh "$top/cloudbsd/make.sh" configure-tree \
    >"$out" 2>&1 || true
assert_has 'the refusal names the variable the user set' "$out" \
    'make.sh: MK_LLVM_CONFIG is not an absolute path'
assert_lacks 'the refusal does not name the other variable' "$out" \
    'make.sh: LLVM_CONFIG is not an absolute path'

out=$fixtures/usage.out
sh "$top/cloudbsd/make.sh" >"$out" 2>&1 || true
assert_has 'the usage names configure-tree' "$out" 'configure-tree'
assert_no_long_line 'no line of the usage is wider than 80 columns' "$out" 80

assert_has 'the README names configure-tree' "$top/cloudbsd/README.md" \
    'configure-tree'

if [ "$fail" -eq 0 ]; then
	printf '%s\n' 'test-make-checks: ok'
else
	printf '%s\n' 'test-make-checks: FAILED'
	exit 1
fi
