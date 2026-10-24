#!/usr/bin/env bash
# Copyright 2026 PolitePixels Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -uo pipefail

# The leg id this script reports under.
LEG=L1

# The CUDA compiler this leg drives, which a reader may override. Set by main when unset.
NVCC="${NVCC:-}"

# The CCCL include root the three programs are compiled against. Set by main.
CCCL=

# The device architecture nvcc targets, which a reader may override.
ARCH="${CCCL_ARCH:-sm_86}"

# Number of assertions performed, reported as the receipt's case count.
cases=0

# Number of assertions that failed.
fails=0

# say prints the leg's receipt line.
# Globals:
#   LEG - Read
# Arguments:
#   $1 - PASS, FAIL or SKIP
#   $2 - The case count
#   $3 - The skipped count
#   $4 - The reason token
# Outputs:
#   One RL-STATUS line on stdout
say() { printf 'RL-STATUS leg=%s result=%s cases=%s skipped=%s reason=%s\n' "$LEG" "$1" "$2" "$3" "$4"; }

# find_nvcc locates a CUDA compiler.
# Outputs:
#   The compiler's path on stdout
# Returns:
#   0 when a compiler was found, 1 otherwise
find_nvcc() {
    local directory
    [ -n "${NVCC:-}" ] && [ -x "${NVCC}" ] && { printf '%s' "$NVCC"; return 0; }
    command -v nvcc >/dev/null 2>&1 && { command -v nvcc; return 0; }
    for directory in /usr/local/cuda/bin /opt/cuda/bin \
             "$HOME"/.local/lib/python*/site-packages/nvidia/cu*/bin \
             /usr/lib*/python*/site-packages/nvidia/cu*/bin; do
        [ -x "$directory/nvcc" ] && { printf '%s' "$directory/nvcc"; return 0; }
    done
    return 1
}

# find_cccl locates the CCCL headers. An explicit CCCL_ROOT always wins.
# Outputs:
#   The include root on stdout
# Returns:
#   0 when the headers were found, 1 otherwise
find_cccl() {
    local directory
    [ -n "${CCCL_ROOT:-}" ] && [ -d "${CCCL_ROOT}/cub" ] && { printf '%s' "$CCCL_ROOT"; return 0; }
    for directory in /usr/local/cuda/include/cccl /usr/include/cccl \
             "$HOME"/.local/lib/python*/site-packages/nvidia/cu*/include/cccl \
             /usr/lib*/python*/site-packages/nvidia/cu*/include/cccl; do
        [ -d "$directory/cub" ] && { printf '%s' "$directory"; return 0; }
    done
    return 1
}

# compile compiles one program, discarding the object.
# Globals:
#   NVCC, CCCL, ARCH - Read
# Arguments:
#   $1 - The source file
# Returns:
#   nvcc's own exit status
compile() { "$NVCC" -std=c++20 -I"$CCCL" -arch="$ARCH" -c "$1" -o /dev/null; }

# expect_clean_compile asserts that a program compiles without a diagnostic.
# Globals:
#   cases - Incremented
#   fails - Incremented on violation
# Arguments:
#   $1 - The source file
# Outputs:
#   One ok or FAIL line, and on failure the first three errors
expect_clean_compile() {
    local file="$1" out rc
    out="$(compile "$file" 2>&1)"; rc=$?
    cases=$((cases + 1))
    if [ "$rc" -ne 0 ]; then
        printf 'FAIL  %-26s was expected to compile and did not\n' "$file"
        printf '%s\n' "$out" | grep -iE 'error' | head -3 | sed 's/^/      /'
        fails=$((fails + 1))
        return
    fi
    cases=$((cases + 1))
    if [ -z "$out" ]; then
        printf 'ok    %-26s compiles without a word, as the paper claims\n' "$file"
    else
        printf 'FAIL  %-26s compiled, but not silently; the claim is that composing two\n' "$file"
        printf '      determinism levels draws no diagnostic at all, and a warning here would\n'
        printf '      be the library beginning to ask the question\n'
        printf '%s\n' "$out" | head -3 | sed 's/^/      /'
        fails=$((fails + 1))
    fi
}

# expect_rejection asserts that a program is rejected, and for the stated reason.
# Globals:
#   cases - Incremented once, or twice when the program was rejected
#   fails - Incremented on violation
# Arguments:
#   $1 - The source file
#   $2 - A substring the diagnostic must carry
# Outputs:
#   One ok or FAIL line, and on failure the first three errors
expect_rejection() {
    local file="$1" wanted="$2" out rc
    out="$(compile "$file" 2>&1)"; rc=$?
    cases=$((cases + 1))
    if [ "$rc" -eq 0 ]; then
        printf 'FAIL  %-26s was expected to be REJECTED and compiled\n' "$file"
        printf '      the local check this leg relies on is not firing, so the counterexample\n'
        printf '      below would be evidence of nothing\n'
        fails=$((fails + 1))
        return
    fi
    cases=$((cases + 1))
    if printf '%s' "$out" | grep -qiF "$wanted"; then
        printf 'ok    %-26s rejected, and for the stated reason\n' "$file"
    else
        printf 'FAIL  %-26s rejected, but not with the expected diagnostic (%s)\n' "$file" "$wanted"
        printf '%s\n' "$out" | grep -iE 'error' | head -3 | sed 's/^/      /'
        fails=$((fails + 1))
    fi
}

# run_programs compiles the two controls and the counterexample, in that order.
# Globals:
#   NVCC, CCCL - Read
# Outputs:
#   The toolchain in use, then one line per assertion
run_programs() {
    printf 'nvcc: %s\nCCCL: %s\n' "$NVCC" "$CCCL"
    printf 'RL-NOTE nvcc-version=%s cccl-version=%s\n' \
        "$("$NVCC" --version 2>/dev/null | sed -n 's/.*release \([0-9.]*\).*/\1/p' | head -1)" \
        "$(sed -n 's/^#define CCCL_VERSION \([0-9]*\).*/\1/p' "$CCCL/cuda/std/__cccl/version.h" 2>/dev/null | head -1)"
    expect_rejection positive_unsatisfiable.cu "gpu_to_gpu determinism is unsupported"
    expect_rejection positive_no_require.cu    "inside requires to have an effect"
    expect_clean_compile counterexample.cu
}

# check_floor asserts that every assertion ran.
#
# It is not an assertion of its own and must not increment `cases`: a check that incremented the
# count and then tested it would be satisfied on every path that reached the comparison, an
# assertion about itself. The leg makes six independently falsifiable assertions, and this
# function guards that all six ran.
# Globals:
#   cases - Read
#   fails - Incremented on violation
# Outputs:
#   One FAIL line when fewer than six assertions ran
check_floor() {
    if [ "$cases" -lt 6 ]; then
        printf 'FAIL  only %s assertion(s) ran, expected 6\n' "$cases"
        fails=$((fails + 1))
    fi
}

# report prints the leg's receipt.
# Globals:
#   cases, fails - Read
# Returns:
#   Exits 1 when any assertion failed, 0 otherwise
report() {
    if [ "$fails" -gt 0 ]; then
        say FAIL "$cases" 0 cccl-composition-check-broken
        exit 1
    fi
    say PASS "$cases" 0 cccl-composes-without-diagnostic
    exit 0
}

# main finds the toolchain and runs the three programs through it.
# Outputs:
#   One line per assertion, then the leg's receipt
# Returns:
#   0 when CCCL composes without a diagnostic, 1 on failure, 77 when the toolchain is absent
main() {
    cd "$(dirname "$0")" || { say FAIL 0 0 no-repo-root; exit 1; }
    NVCC="$(find_nvcc)"       || { say SKIP 0 0 no-cccl; exit 77; }
    CCCL="$(find_cccl)"       || { say SKIP 0 0 no-cccl; exit 77; }
    run_programs
    check_floor
    report
}

main "$@"
