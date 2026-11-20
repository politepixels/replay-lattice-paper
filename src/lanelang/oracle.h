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

#ifndef LANELANG_ORACLE_H
#define LANELANG_ORACLE_H

#include "dag.h"
#include "lane.h"

// oracle_lane computes the reference lane by enumerating operand paths in an acyclic
// graph. It returns LANE_NONE if the node is out of range or the walk exceeds its limit.
//
// Takes g (dag_t) which holds the graph.
// Takes node (int) which is the index of the node to check.
// Takes cap (long) which is the largest number of complete paths accepted.
// Takes paths_out (long) which receives the number of complete paths visited for a valid
// node, if supplied.
lane_t oracle_lane(const dag_t *g, int node, long cap, long *paths_out);

#endif  // LANELANG_ORACLE_H
