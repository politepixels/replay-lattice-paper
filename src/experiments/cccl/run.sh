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
cd "$(dirname "$0")/../../.." || exit 1

# shellcheck source=../../../scripts/lib/receipts.sh
. ./scripts/lib/receipts.sh

# Where the three programs live.
EXPERIMENT_DIR="src/experiments/cccl"

# The CUDA compiler this leg drives, which a reader may override. Set by require_toolchain.
NVCC="${NVCC:-}"

# The CCCL include root the three programs are compiled against. Set by require_toolchain.
CCCL=""

# The device architecture nvcc targets, which a reader may override.
ARCH="${CCCL_ARCH:-sm_86}"

# How many assertions this gate has made so far.
cases=0

# How many assertions failed. Every program is still compiled after a failure, so the run
# shows each verdict the toolchain gave.
fails=0

# say prints the machine-readable receipt and exits with the code the protocol gives it.
# Arguments:
#   $1 - PASS, SKIP or FAIL
#   $2 - How many cases the gate asserted
#   $3 - How many it declined
#   $4 - The reason token
# Outputs:
#   The RL-STATUS line on stdout
# Returns:
#   Exits 0 for PASS, 77 for SKIP and 1 otherwise
say() {
    printf 'RL-STATUS leg=L1 result=%s cases=%s skipped=%s reason=%s\n' "$1" "$2" "$3" "$4"
    case "$1" in
        PASS) exit 0 ;;
        SKIP) exit 77 ;;
        *)    exit 1 ;;
    esac
}

# find_nvcc locates a CUDA compiler.
# Globals:
#   NVCC - Read
# Outputs:
#   The compiler's path on stdout
# Returns:
#   0 when a compiler was found, 1 otherwise
find_nvcc() {
    local directory
    [ -n "$NVCC" ] && [ -x "$NVCC" ] && { printf '%s' "$NVCC"; return 0; }
    command -v nvcc >/dev/null 2>&1 && { command -v nvcc; return 0; }
    for directory in /usr/local/cuda/bin /opt/cuda/bin \
             "${VIRTUAL_ENV:-/nonexistent}"/lib/python*/site-packages/nvidia/cu*/bin \
             "$HOME"/.local/lib/python*/site-packages/nvidia/cu*/bin \
             /usr/lib*/python*/site-packages/nvidia/cu*/bin; do
        [ -x "$directory/nvcc" ] && { printf '%s' "$directory/nvcc"; return 0; }
    done
    return 1
}

# find_cccl looks first in CCCL_ROOT for a cub directory, then in the standard install paths
# and an active virtual environment.
# Outputs:
#   The include root on stdout
# Returns:
#   0 when the headers were found, 1 otherwise
find_cccl() {
    local directory
    [ -n "${CCCL_ROOT:-}" ] && [ -d "${CCCL_ROOT}/cub" ] && { printf '%s' "$CCCL_ROOT"; return 0; }
    for directory in /usr/local/cuda/include/cccl /usr/include/cccl \
             "${VIRTUAL_ENV:-/nonexistent}"/lib/python*/site-packages/nvidia/cu*/include/cccl \
             "$HOME"/.local/lib/python*/site-packages/nvidia/cu*/include/cccl \
             /usr/lib*/python*/site-packages/nvidia/cu*/include/cccl; do
        [ -d "$directory/cub" ] && { printf '%s' "$directory"; return 0; }
    done
    return 1
}

# require_toolchain skips the gate when nvcc or the CCCL headers are absent, because a check
# that cannot run must say so and never report PASS.
# Globals:
#   NVCC, CCCL - Set
# Outputs:
#   The SKIP receipt when either is missing
require_toolchain() {
    NVCC="$(find_nvcc)" || say SKIP 0 1 no-cccl
    CCCL="$(find_cccl)" || say SKIP 0 1 no-cccl
}

# compile compiles one program, discarding the object.
# Globals:
#   NVCC, CCCL, ARCH - Read
# Arguments:
#   $1 - The source file
# Returns:
#   nvcc's own exit status
compile() {
    "$NVCC" -std=c++20 -I"$CCCL" -arch="$ARCH" -c "$1" -o /dev/null
}

# expect_clean_compile asserts that a program compiles without a diagnostic.
# Globals:
#   EXPERIMENT_DIR - Read
#   cases - Incremented once, or twice when compilation succeeds
#   fails - Incremented on violation
# Arguments:
#   $1 - The source file's name
# Outputs:
#   The verdict and, on failure, up to three lines of compiler output
expect_clean_compile() {
    local file="$1" out rc
    out="$(compile "$EXPERIMENT_DIR/$file" 2>&1)"
    rc=$?
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
        printf 'FAIL  %-26s compiled, but not silently. The claim is that composing two\n' "$file"
        printf '      determinism levels draws no diagnostic at all, and a warning here would\n'
        printf '      be the library beginning to ask the question\n'
        printf '%s\n' "$out" | head -3 | sed 's/^/      /'
        fails=$((fails + 1))
    fi
}

# expect_rejection asserts that a program is rejected, and for the stated reason.
# Globals:
#   EXPERIMENT_DIR - Read
#   cases - Incremented once, or twice when the program was rejected
#   fails - Incremented on violation
# Arguments:
#   $1 - The source file's name
#   $2 - A substring the diagnostic must carry
# Outputs:
#   The verdict and up to three error lines when rejection has the wrong diagnostic
expect_rejection() {
    local file="$1" wanted="$2" out rc
    out="$(compile "$EXPERIMENT_DIR/$file" 2>&1)"
    rc=$?
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
#   The toolchain in use and a verdict for each program
run_programs() {
    printf 'nvcc: %s\nCCCL: %s\n' "$NVCC" "$CCCL"
    printf 'RL-NOTE nvcc-version=%s cccl-version=%s\n' \
        "$("$NVCC" --version 2>/dev/null | sed -n 's/.*release \([0-9.]*\).*/\1/p' | head -1)" \
        "$(sed -n 's/^#define CCCL_VERSION \([0-9]*\).*/\1/p' "$CCCL/cuda/std/__cccl/version.h" \
            2>/dev/null | head -1)"
    expect_rejection positive_unsatisfiable.cu "gpu_to_gpu determinism is unsupported"
    expect_rejection positive_no_require.cu "inside requires to have an effect"
    expect_clean_compile counterexample.cu
}

# check_floor asserts that every assertion ran.
#
# Each of the three programs must compile or fail as expected before its output is
# checked. A complete run therefore counts six assertions before this check.
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

# report prints the receipt for the whole run.
# Globals:
#   cases, fails - Read
# Outputs:
#   The receipt on stdout
report() {
    [ "$fails" -eq 0 ] || say FAIL "$cases" 0 cccl-composition-check-broken
    say PASS "$cases" 0 cccl-composes-without-diagnostic
}

# main finds the toolchain, runs the three programs through it and reports.
# Outputs:
#   The toolchain and program verdicts, then the receipt on stdout
main() {
    require_toolchain
    run_programs
    check_floor
    report
}

main "$@"
