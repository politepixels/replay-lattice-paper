# The Replay Lattice

**A language in which *these bits must not depend on the schedule* is a sentence the compiler checks.**

Floating-point addition is not associative, so a parallel reduction's result depends on how many workers folded it. Almost every remedy the field has built is a **grant attached to a site**: an LLVM `reassoc` flag, an MLIR `fastmath` attribute, a per-call `require(determinism::run_to_run)`. A grant says what *may* happen here, and grants compose trivially.

Reproducibility is the opposite. It is a **demand attached to a value**: *these bits must not depend on the schedule*, satisfied only if every operation that ever touched the value obeyed it. Demands do not compose. That is why a per-call knob cannot express the property, and it is what this repository is about.

The artefact is `src/lanelang/`: a small language whose every value carries a schedule-class
demand (`exact` / `replay` / `approx`), joined over its whole dependency path and checked at each
declaration, with the soundness argument mechanised in `src/proofs/` and one experiment inside
NVIDIA's own toolchain in `src/experiments/cccl/`. The paper describes the model and the
checks on the C implementation. [The Replay Lattice: A Type System for Floating-Point Reproducibility](PAPER.pdf)

## Quickstart

Build with a C11 compiler, POSIX threads and Make; no CMake configuration is needed. Run the commands below
from the repository root. The five main executables are written to `build/bin/`.

```sh
git clone https://github.com/politepixels/replay-lattice-paper && cd replay-lattice-paper
make build             # build LaneLang and the test tools
make test              # fixture and command-line tests
./build/bin/lanelang run src/tests/lanelang/accept_ordered.lane
```

For the paper's full evaluation and deliberately broken implementations:

```sh
make reproduce         # clean rebuild and the numbered checks
make controls          # verify that each deliberate break is detected
make check-iris        # check the concurrent sum proof in the Rocq and Iris environment
```

Coq (`coqc`) enables the proof checks; CUDA (`nvcc`) with CCCL headers enables the CUDA
experiment. `reproduce` can skip missing tools; `controls` fails if a control cannot run.
A successful control reports `FIRED`: the deliberate defect was detected.

## Use LaneLang on one program

All four modes take a `.lane` file. Use an existing fixture or substitute your own program:

```sh
./build/bin/lanelang check src/tests/lanelang/accept_ordered.lane  # check its demands
./build/bin/lanelang run   src/tests/lanelang/accept_ordered.lane  # compute and print its result
./build/bin/lanelang lower src/tests/lanelang/accept_ordered.lane  # show selected kernels
./build/bin/lanelang dot   src/tests/lanelang/accept_ordered.lane  # print the graph as DOT text
```

To see a rejection and the diagnostic linking a demand to its upstream producer:

```sh
./build/bin/lanelang check src/tests/lanelang/reject_poison.lane
```

That command intentionally exits with status `1`. In the fixture suite, an expected rejection
is a passing test. Fixtures beginning `malformed_` test invalid source instead.

To render the graph as an image, install Graphviz and pipe the DOT text into its renderer:

```sh
./build/bin/lanelang dot src/tests/lanelang/accept_ordered.lane | dot -Tsvg > /tmp/lanelang.svg
```

`run` checks the program, then interprets its healed graph and prints the value returned by
`main`. A scalar prints on one line. A column prints one element per line in index order.
Each number has up to 17 significant digits. Diagnostics go to stderr. The command uses the
schedule tests' evaluator with materialised intermediates, one worker and the default input world.
`source()` uses that world's generated test data. Sources and generators support lengths from
1 through 100,000. A request outside that range exits with status 4.

`lower` prints a kernel mapping. Both commands operate on the checked graph without producing a
standalone executable.

The Make wrapper builds the tools if needed and accepts `ACTION=check|run|lower|dot`:

```sh
make lanelang ACTION=run FILE=src/tests/lanelang/accept_ordered.lane
```

Execution always checks the program, even if the inspection control `LANELANG_NOCHECK=1`
is set. The binary exits with `0` on success, `1` for a rejected demand, `2` for malformed
source, `3` for command/file errors, and `4` if evaluation fails (for example, a checksum
mismatch or an integer sum outside its exact domain). A failed run prints no result.

## What each lane lets the compiler optimise

One function in the fixture suite computes `sum(x)` twice. The ordinary binding lowers to
`fold_free`, while `replay b := sum(x)` lowers to `fold_replay`. A reproducibility demand on
`b` leaves the other sum free to regroup its additions. [LaneLang example](src/tests/lanelang/accept_cse_distinct.lane)

The evaluator's `approx` sum spreads each worker's chunk across 16 independent accumulators,
which the compiler can pack into SIMD registers. On x86-64 Linux with glibc, the GCC and Clang
builds select an SSE2, AVX2 or AVX-512 kernel at run time. On Arm64 the compiler can use NEON.
The `replay` sum adds corresponding elements of two 64-element blocks side by side. This fits
both SSE2 and NEON, preserves the order within each block and combines block totals in input
order. The `exact` sum uses one accumulator and visits elements from left to right.

| Lane | Reduction on a materialised column | Full evaluator |
|---|---:|---:|
| `approx` | 11.63 µs | 637.51 µs |
| `replay` | 21.74 µs | 643.00 µs |
| `exact` | 60.82 µs | 635.58 µs |

Each time is the median of 25 batches summing 100,000 doubles on one performance core of an
Intel Core Ultra 7 155U with GCC 15.3.1 at `-O3` and floating-point contraction disabled.
On this machine `approx` used AVX2 and `replay` used SSE2. The reduction reuses a warm column.
The full evaluator generates the column and sums it on each call. Its three medians were within
1.2% of one another in this run. Both measurements exclude parsing and checking. The paper
specifies the guarantees each lane must meet. These timings reflect one choice of summation
algorithms on this input and machine.

Run the comparison with `make bench-sums`. On Linux, `taskset -c 0 make bench-sums` also pins
the run to CPU 0, the performance core used here.

## Run individual checks

Build first with `make build` before invoking a binary directly. `make test` covers fixtures
and CLI behaviour; the other checks below run separately or through `make reproduce`.

| Command | What it checks |
|---|---|
| `make test-cli` | Command arguments, execution results, diagnostics and exit codes |
| `./build/bin/test_lanelang src/tests/lanelang` | Fixture verdicts, blame diagnostics and kernel choices |
| `./build/bin/test_schedule` | Numerical results across schedules, real worker participation in the replay sum, and agreement of exact folds with the serial reference |
| `./build/bin/test_bless` | Acceptance of matching blessed data and refusal of a changed checksum |
| `./scripts/check-proofs.sh` | [Type-system proofs and their declared assumptions](src/proofs/ReplayLattice.v) (requires Coq) |
| `./scripts/check-proofs-eval.sh` | [Evaluator kernels](src/proofs/ReplayLatticeEval.v) and [carried agreement for nested scalar loops](src/proofs/ScalarRecurrence.v) (requires Coq) |
| `make check-iris` | [Concurrent fixed-tree sum](src/proofs/ConcurrentReplay.v) (Rocq 9.1, Stdlib 9.1 and Iris 4.5) |
| `./src/experiments/cccl/run.sh` | Compile-only CUDA/CCCL composition experiment |

The standalone Iris check models a fixed-tree sum with any positive number of workers.
Every terminating interleaving returns the existing binary64 model's result, and every input
admits a terminating schedule. In an opam switch with the Rocq released repository, run
`opam install rocq-core.9.1.0 rocq-stdlib.9.1.0 rocq-iris.4.5.0`,
then `opam exec -- make check-iris`.

Fuzzing generates many cases. These commands use 2,000 samples and seed `1`:

```sh
make fuzz MODE=roundtrip COUNT=2000 SEED=1    # check parsed and healed graphs against the path oracle
make fuzz MODE=agree COUNT=2000 SEED=1        # fast analysis versus exhaustive path enumeration
make fuzz MODE=metamorphic COUNT=2000 SEED=1  # properties under controlled graph changes
```

The direct equivalent is `./build/bin/lanefuzz --roundtrip 2000 1`; substitute `--agree` or
`--metamorphic` for the other modes. Reuse the same seed to repeat a case set. The Make defaults
are `MODE=roundtrip`, `COUNT=2000`, `SEED=20260725`. Very small samples can fail coverage
requirements even when all comparisons agree. The fuzzer prints results, not generated source.

`reproduce.sh` reports `PASS`, `FAIL` or `SKIP` for each check and names skips in its final tally.
Use `./reproduce.sh --list` to see the checks and accepted skip tags; passing
`--expect-skips=TAG1,TAG2` makes any undeclared skip fail the run. [ARTIFACT.md](ARTIFACT.md)
maps every paper claim to its command and expected output.

`make help` lists the root targets. Plain `make` builds the tools; `make clean` removes build outputs.
