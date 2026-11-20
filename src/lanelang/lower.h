// Copyright 2026 PolitePixels Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LANELANG_LOWER_H
#define LANELANG_LOWER_H

#include "dag.h"

// lower_kernel_for is the kernel `node` lowers to, or NULL when it is not a fold or a
// scan.
//
// Takes node (dagnode_t) which is the node under examination.
const char *lower_kernel_for(const dagnode_t *node);

// lower_kernel_demand_free_cases checks that varying the demand and ambient lane leaves
// kernel selection unchanged for each class. It returns the number of triples checked, or
// zero on a mismatch.
int lower_kernel_demand_free_cases(void);

// lower_print prints one line for each kernel-bearing node in `g`, giving the node index,
// its label and the selected kernel. An unlabelled node uses "sum".
//
// Takes g (dag_t) which is the graph being walked.
// Takes stream (void) which is where the output is written.
int lower_print(const dag_t *g, void *stream);

#endif  // LANELANG_LOWER_H
