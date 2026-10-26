# Artefact: claim to command

_Every claim in the paper, the command that reproduces it, and what you should see._

Run everything at once with `./reproduce.sh` and `./controls.sh`. Together they take about a minute
on a laptop; a machine with `coqc` takes longer. This file exists so an individual claim can be
checked without reading the harness.

## Prerequisites

A C11 compiler and `make`. `coqc` enables the two proof checks; `nvcc` with the CCCL headers enables
the CCCL experiment. Each is optional, and each check skips by name when its prerequisite is absent,
never reporting PASS. Without `coqc`, L2, which carries nine of the operation-class table's eleven
rows, skips with L9, and L3 and L4 still witness seven of those nine; the elementwise and
data-movement rows then have no witness. `./controls.sh` needs `coqc`, because two of its controls
break the proofs, and a control that cannot run fails the run.

There is no build system to configure and no dependency to install.

## The checks

Every check the harness runs has a row here, and every row here is a check the harness runs. The
table used to list ten while the harness ran nineteen, which is the same defect as everything else
this artefact reports: two representations of one fact with nothing forcing them to agree.

Each check prints one machine-readable line, `RL-STATUS leg=<id> result=... cases=... reason=...`;
the `leg=` field carries the check's id.

| Check | Claim | Command | Expected |
|---|---|---|---|
| L1 | CCCL answers the local determinism question and cannot ask the composition one: two programs that must be rejected, and a `not_guaranteed` result feeding a `run_to_run` call that compiles cleanly | `./src/experiments/cccl/run.sh` | `result=PASS cases=6 reason=cccl-composes-without-diagnostic`. Compile-only; needs `nvcc` and the CCCL headers (`pip install nvidia-cuda-nvcc nvidia-cuda-cccl`), else `SKIP no-cccl` |
| L2 | Section 3.5's operation-class rows, proved of the model evaluator, each invariance row paired with a witness that the same machinery separates schedules | `./scripts/check-proofs-eval.sh` | `result=PASS cases=2 reason=-`. Skips by name without Coq (tag `coq`) |
| L3 | 171 checker fixtures: verdicts, lanes, blame spans, kernel picks, ungrounded-node counts, and the fixed point's pass bounds | `make -C src/lanelang all && ./build/bin/test_lanelang src/tests/lanelang` | `result=PASS cases=751` |
| L4 | A pinned value's bits do not move across traversals and worker counts, evaluated on the healed graph and only where accepted; an `exact` value equals the serial reference; a free scan under a pinned fold still moves | `./build/bin/test_schedule` | `result=PASS cases=639 reason=-`: the incident's free sum keeps the recorded bits at 1 of 4 worker counts, and the rest move it first in significant digit 8 |
| L5 | A blessed checksum is a receipt: a parsed `@bless(n)` with world 1's checksum, evaluated in world 2, is refused by the checksum and not the world | `make -C src/lanelang all && ./build/bin/test_bless` | `result=PASS cases=5` |
| L6 | The lattice answer is the same over programs the front end parsed, resolved and healed as over hand-built graphs | `./build/bin/lanefuzz --roundtrip 2000` | `result=PASS cases=47740 reason=-`: 47,738 node answers with 0 mismatches; healing strengthened 371 producers and moved 2,809 answers |
| L7 | The linear join agrees with exhaustive path enumeration, and refuses a graph whose storage order it cannot analyse | `./build/bin/lanefuzz --agree 2000` | `result=PASS cases=60245`: 58,245 node answers with 0 mismatches, plus 2,000 order-permuted graphs the sweep must refuse |
| L8 | The join sweep is monotone, order-respecting, and non-mutating | `./build/bin/lanefuzz --metamorphic 2000` | `result=PASS cases=8000`: three properties over 2000 random programs |
| L9 | The theorems of sections 3 to 5, mechanised, resting on exactly the eight declared axioms; an admitted proof fails the audit as an extra axiom | `./scripts/check-proofs.sh` | `result=PASS cases=2 reason=-`. Skips by name without Coq (tag `coq`) |
| L10 | The harness catches its own breaks: an undeclared skip, a check asserting nothing, a stub with no status line, a drifted ledger count | `./reproduce.sh --self-check` | `THE HARNESS CATCHES ITS OWN BREAKS (5 controls, all fired)`, `result=PASS cases=5`. Runs first |
| L11 | Everything runs from a clean tree | `./reproduce.sh` cleans and rebuilds before any check | `result=PASS cases=1 reason=built` |
| L12 | Every check binary was freshly built and says so | each binary prints a `BUILD` line naming its compiler, `std=` and `double=` | `result=PASS cases=4 reason=freshly-built` |

## The controls

These are the ones to run if you only run one thing. Each deliberately breaks the system and
requires the corresponding check to notice; a control that does not fire means the check it guards
is decorative.

```sh
./controls.sh
```

| Control | The break | You should see |
|---|---|---|
| CC1 | The grounding judgement removed | `FIRED: without grounding, 9 demand(s) over an external source were accepted` |
| CC2 | The fixed tree's bracketing made to follow the worker count, so the row saying its bits are a function of `(data, shape)` stops being true | `FIRED: the fixed tree made to read the worker count; the row is no longer provable (proof-does-not-compile)` |
| CC3 | The fixed-block scan's frozen leaf width made to follow the worker count | `FIRED: letting the worker count pick the scan's leaf width moved N pinned value(s)` |
| CC4 | `exact` discharged by the fixed tree rather than the oracle order | `FIRED: the fixed tree discharged exact with all 18 schedule-invariance assertion(s) still green; only the reference comparison saw it` |
| CC5 | the contract removed from the value-numbering key | `FIRED: twins collapsed [fold_free] where the contract keeps them distinct [fold_free fold_replay]` |
| CC6 | a malformed program | `FIRED: reported malformed (exit 2), not as a lane violation` |
| CC7 | The evaluator made to fail a pinned fold after the checker accepted the program | `FIRED: an accepted pinned program failed to evaluate at N lane(s); L4 reported it as a failure, not as a refusal` |
| CC8 | the checker kill switch | `FIRED: suppresses the verdict and changes no kernel pick` |
| CC9 | The three lane words and `ungrounded` left unreserved, so the prefix is decided by lookahead and a word with no name after it is a name | `FIRED: unreserved qualifiers bound N nameless demand(s) as variables and lost M expected diagnostic(s)` |
| CC10 | A blessing that marks the leaf grounded rather than excusing the declaration | `FIRED: marking a blessed leaf grounded repaired 8 cone(s) with no accept-or-reject verdict changed` |
| CC11 | The reorder dial aliased onto the demand, so `order: oracle` states an `exact` demand the source did not write | `FIRED: aliasing the dial onto exact survived 4 of the 5 cross-product fixtures; xp_dial_only caught it, because the free kernel it requires was healed away by a demand the source never wrote` |
| CC12 | A demand stamped onto the value instead of carried on a node of its own, which is what put `declared` in the interning key after interning | `FIRED: a stamped demand refused N value(s) the shipped build accepts, a promise read in a wider expression among them, and lost N pinned kernel(s)` |
| CC13 | Lowering reads the kernel from the demand field instead of the class fixed at resolution | `FIRED: the demand-driven discharge was caught by the invariant sweep alone, with no verdict changed` |
| CC14 | The tile width allowed to decide the bracketing, which is reduction factoring disguised as tiling | `FIRED: letting the tile width decide the bracketing moved N pinned value(s) that must not move` |
| CC15 | The sweep's ordering guard reverted to a fail-open skip | `FIRED: the fail-open guard answered 2000 out-of-order graph(s) instead of refusing them` |
| CC16 | A lemma inside Theorem 1's dependency cone admitted rather than proved | `FIRED: the admitted lemma surfaced as an undeclared axiom (unexpected-axiom-set)` |
| CC17 | healing disabled | `FIRED: accept_healed ... rejected without it` |
| CC18 | The structural restriction on `@bless` dropped, so a waiver binds to a value the program computes | `FIRED: with the restriction dropped, a waiver bound to a fold the program computes` |
| CC19 | An `ungrounded` declaration made to state nothing, so the grounding answer it carries is the ordinary conjunction over its body | `FIRED: an empty ungrounded declaration accepted N refused demand(s) and dropped M ungrounded-node count(s)` |

## The audit's other occasions

<!-- rl:assert count green-checks=ten -->
The paper's §7.2 counts ten occasions on which a passing check had proved nothing and could not
have failed on the defect it was written to catch, and describes the four that bear on the type
system. These are the other six.

| # | Occasion | Repair |
|---|---|---|
| 5 | The predecessor's single generator gave one bit pattern under every fold order, so no order assertion over it could fail | The generators are order-sensitive by construction |
| 6 | Metamorphic property M2 compared against `approx`, whose count M1 asserts is zero, so it could only fail once M1 had; a second half leaned on the same zero | M2 compares against `replay`, and L8's count fell from 10,000 to 8,000 |
| 7 | A build treating `order: oracle` as `exact` survived four of the five fixtures written to tell them apart | `xp_dial_only` catches it, and CC11 builds the alias |
| 8 | The blocked scan's first implementation accumulated across block boundaries, so its published leaf width moved no bits | Found because CC3 could not fire; each block's prefix restarts at zero |
| 9 | L3 asserted at most three passes, and no fixture nested a recurrence | The ceiling is `d + 2`, `d` being the most carried edges a change must cross |
| 10 | The only evaluated recurrence was flat, and the first nested one answered 10 where 24 is right, because the inner tick count was not reset | The evaluator resets it, and L4 evaluates nested recurrences |

## Checking one claim by hand

**The silent mix is rejected, and the blame points at the producer:**

```sh
./build/bin/lanelang check src/tests/lanelang/reject_poison.lane
```

**The same program is accepted once the producer is repaired:**

```sh
./build/bin/lanelang check src/tests/lanelang/accept_reannotate.lane
```

**Two structurally identical reductions get two different kernels:**

```sh
./build/bin/lanelang lower src/tests/lanelang/accept_cse_distinct.lane
```

**The DAG, as a figure** (this is how the paper's figures are produced, so a figure cannot silently
disagree with the implementation):

```sh
./build/bin/lanelang dot src/tests/lanelang/reject_mix.lane | dot -Tsvg > mix.svg
```