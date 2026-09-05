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

CC      	?= cc
CFLAGS  	:= -O2 -std=c11 -Wall -Wextra -Werror
BUILD   	?= ../../build
BIN     	:= $(BUILD)/bin
INCLUDES 	:= -I.
CORE 		:= diagnostic.c lexer.c parse.c parse_expr.c dag.c resolve.c builtins.c bodies.c heal.c lattice.c blame.c lower.c eval.c kernels.c generator.c
ORACLE  	:= oracle.c
HEADERS 	:= lane.h diagnostic.h lexer.h ast.h parse.h parse_internal.h dag.h resolve.h resolve_internal.h heal.h lattice.h lower.h eval.h kernels.h generator.h status.h

.PHONY: all controls test clean
.SUFFIXES:

all: $(BIN)/lanelang $(BIN)/test_lanelang $(BIN)/lanefuzz $(BIN)/test_schedule $(BIN)/test_bless

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/lanelang: main.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ main.c $(CORE)

$(BIN)/test_lanelang: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_lanelang.c $(CORE)

$(BIN)/lanefuzz: fuzz.c $(CORE) $(ORACLE) $(HEADERS) oracle.h | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ fuzz.c $(CORE) $(ORACLE)

$(BIN)/test_schedule: test_schedule.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_schedule.c $(CORE)

$(BIN)/test_bless: test_bless.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_bless.c $(CORE)

$(BIN)/lanelang_noheal: main.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_NO_HEAL -o $@ main.c $(CORE)

$(BIN)/lanelang_nolanecse: main.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_CSE_IGNORES_LANE -o $@ main.c $(CORE)

$(BIN)/test_lanelang_demandkernel: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_KERNEL_FOLLOWS_DEMAND -o $@ test_lanelang.c $(CORE)

$(BIN)/test_lanelang_orderedalias: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_ORDERED_ALIASES_EXACT -o $@ test_lanelang.c $(CORE)

$(BIN)/test_schedule_scanblock: test_schedule.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_SCAN_BLOCK_FOLLOWS_WORKERS -o $@ test_schedule.c $(CORE)

$(BIN)/test_lanelang_nogrounding: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_NO_GROUNDING -o $@ test_lanelang.c $(CORE)

$(BIN)/test_schedule_exacttree: test_schedule.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_EXACT_LOWERS_TO_TREE -o $@ test_schedule.c $(CORE)

$(BIN)/test_lanelang_blessgrounds: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_BLESS_FLIPS_GROUNDED -o $@ test_lanelang.c $(CORE)

$(BIN)/test_lanelang_blessany: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_BLESS_ON_ANY_NODE -o $@ test_lanelang.c $(CORE)

$(BIN)/test_lanelang_unreserved: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_QUALIFIERS_UNRESERVED -o $@ test_lanelang.c $(CORE)

$(BIN)/test_lanelang_stampdemand: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_DEMAND_STAMPS_NODE -o $@ test_lanelang.c $(CORE)

$(BIN)/test_lanelang_opaquegrounded: test_lanelang.c $(CORE) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) -DLANELANG_OPAQUE_IS_GROUNDED -o $@ test_lanelang.c $(CORE)

controls: $(BIN)/lanelang_noheal $(BIN)/lanelang_nolanecse $(BIN)/test_lanelang_demandkernel \
          $(BIN)/test_schedule_scanblock $(BIN)/test_lanelang_nogrounding \
          $(BIN)/test_schedule_exacttree \
          $(BIN)/test_lanelang_orderedalias $(BIN)/test_lanelang_blessgrounds \
          $(BIN)/test_lanelang_blessany $(BIN)/test_lanelang_opaquegrounded \
          $(BIN)/test_lanelang_stampdemand $(BIN)/test_lanelang_unreserved

test: all
	$(BIN)/test_lanelang ../tests/lanelang

clean:
	rm -f $(BIN)/lanelang $(BIN)/test_lanelang $(BIN)/lanefuzz $(BIN)/test_schedule \
	      $(BIN)/test_bless
	rm -f $(BIN)/lanelang_* $(BIN)/test_lanelang_* $(BIN)/test_schedule_*
