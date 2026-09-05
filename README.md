# The Replay Lattice

**A language in which *these bits must not depend on the schedule* is a sentence the compiler checks.**

Floating-point addition is not associative, so a parallel reduction's result depends on how many workers folded it. Almost every remedy the field has built is a **grant attached to a site**: an LLVM `reassoc` flag, an MLIR `fastmath` attribute, a per-call `require(determinism::run_to_run)`. A grant says what *may* happen here, and grants compose trivially.

Reproducibility is the opposite. It is a **demand attached to a value**: *these bits must not depend on the schedule*, satisfied only if every operation that ever touched the value obeyed it. Demands do not compose. That is why a per-call knob cannot express the property, and it is what this repository is about.

The artefact is `src/lanelang/`: a small language whose every value carries a schedule-class
demand (`exact` / `replay` / `approx`), joined over its whole dependency path and checked at each
declaration, with the soundness argument mechanised in `src/proofs/` and one experiment inside
NVIDIA's own toolchain in `src/experiments/cccl/`. It exists to make one paper credible,
*The Replay Lattice: A Type System for Floating-Point Reproducibility* (`PAPER.md`), and its checks prove the type system
works, nothing more.

## Quickstart

```sh
git clone https://github.com/politepixels/replay-lattice-paper && cd replay-lattice-paper
./reproduce.sh          # every receipt; Coq and CCCL checks SKIP cleanly where absent
./controls.sh           # the must-fail controls: proof the checks can fail
```

`reproduce.sh` prints one line per check and a final tally that **names its skips**. On a machine with Coq and a CUDA compiler with the CCCL headers, every check runs and the tally ends `ALL CHECKS GREEN`, with nothing skipped. On a box without them those checks still appear in the tally, each named as a skip.

A check that cannot run reports `SKIP`, never `PASS`. `--expect-skips=` takes the tags a runner lacks and turns any *other* skip into a failure, which is how CI stays green on a lean machine without ever being a false pass. `./reproduce.sh --list` prints the tags it accepts, which is where to read them: a list copied into prose is a list that goes stale, and this one did. `ARTIFACT.md` maps every claim to the command that reproduces it.

## Try the checker on one program

```sh
make -C src/lanelang all
./build/bin/lanelang check src/tests/lanelang/reject_poison.lane
```

The second command prints the blame diagnostic *The Replay Lattice* quotes: the site that states the demand, the producer upstream where the guarantee was actually lost, and the distance between them. Swap in any fixture from `src/tests/lanelang/` to see an acceptance, a healing or a different rejection, or write your own `.lane` file.  
`./build/bin/test_lanelang src/tests/lanelang` runs all of them and prints one verdict per fixture.