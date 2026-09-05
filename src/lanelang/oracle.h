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

// oracle_lane is the reference lane for `node` of graph `g`, by path enumeration.
//
// `cap` bounds how many complete paths may be walked. Returns LANE_NONE when `cap` was
// exceeded, in which case the caller must count the DAG as skipped, not as agreeing.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (int) which is the node under examination.
// Takes cap (long) which bounds how much work the walk may do.
// Takes paths_out (long) which receives how many complete paths were walked.
lane_t oracle_lane(const dag_t *g, int node, long cap, long *paths_out);

#endif  // LANELANG_ORACLE_H
