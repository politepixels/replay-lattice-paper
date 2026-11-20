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

.POSIX:

.PHONY: all build help test test-cli lanelang fuzz reproduce controls check-iris bench-sums clean

BUILD ?= build
MODE ?= roundtrip
COUNT ?= 2000
SEED ?= 20260725
ACTION ?= check
FILE ?=

all: build

build:
	@$(MAKE) -C src/lanelang BUILD="$(abspath $(BUILD))" all

help:
	@echo "make build       Build the checker and test tools (default)"
	@echo "make test        Run the LaneLang fixture and CLI suites"
	@echo "make test-cli    Run the LaneLang command-line tests"
	@echo "make lanelang    Use FILE=path/to/program.lane (ACTION=check|run|lower|dot)"
	@echo "make fuzz        Run generated-source comparisons (default: 2000 samples)"
	@echo "                 MODE=agree|roundtrip|metamorphic COUNT=2000 SEED=20260725"
	@echo "make reproduce   Run the complete receipt suite"
	@echo "make controls    Run the must-fail controls"
	@echo "make bench-sums  Compare the three sum kernels and full evaluator timings"
	@echo "make check-iris  Check the concurrent replay sum with Rocq and Iris"
	@echo "make clean       Remove build outputs"

test:
	@$(MAKE) -C src/lanelang BUILD="$(abspath $(BUILD))" test

test-cli:
	@$(MAKE) -C src/lanelang BUILD="$(abspath $(BUILD))" test-cli

lanelang: build
	@test -n "$(FILE)" || { echo "Usage: make lanelang FILE=path/to/program.lane [ACTION=check|run|lower|dot]" >&2; exit 2; }
	@"$(BUILD)/bin/lanelang" "$(ACTION)" "$(FILE)"

fuzz: build
	@"$(BUILD)/bin/lanefuzz" "--$(MODE)" "$(COUNT)" "$(SEED)"

reproduce:
	@./reproduce.sh

controls:
	@./controls.sh

bench-sums:
	@$(MAKE) -C src/lanelang BUILD="$(abspath $(BUILD))" bench-sums

check-iris:
	@./scripts/check-proofs-iris.sh

clean:
	rm -rf "$(BUILD)"
