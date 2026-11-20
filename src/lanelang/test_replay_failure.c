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

#include "kernels.h"
#include "status.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static long cases = 0;

static long failures = 0;

static int fail_alloc;
static int fail_create;
static int launched;
static int joined;

void *test_calloc(size_t count, size_t size) {
    return fail_alloc ? NULL : calloc(count, size);
}

int test_create(pthread_t *thread, const pthread_attr_t *attributes,
                void *(*routine)(void *), void *argument) {
    if (launched == fail_create) {
        return EAGAIN;
    }
    const int status = pthread_create(thread, attributes, routine, argument);
    if (status == 0) {
        launched++;
    }
    return status;
}

int test_join(pthread_t thread, void **result) {
    const int status = pthread_join(thread, result);
    if (status == 0) {
        joined++;
    }
    return status;
}

static void check(const int ok, const char *what) {
    cases++;
    if (!ok) {
        failures++;
        fprintf(stderr, "  FAIL %s\n", what);
    }
}

static int run_fold(eval_state_t *reader, const int fail_at, const int allocation) {
    double out = -1.0;
    launched = 0;
    joined = 0;
    fail_create = fail_at;
    fail_alloc = allocation;
    const int ok = fold_replay(reader, 0, reader->lengths[0], 3, &out);
    if (fail_at >= 0 || allocation) {
        return !ok && out == -1.0 && launched == (allocation ? 0 : fail_at) && joined == launched;
    }
    return ok && out == 1.0 && reader->replay_workers == 3 && launched == 3 && joined == launched;
}

int main(void) {
    double data[129] = {0};
    data[0] = 0x1p54;
    data[64] = -0x1p54;
    data[128] = 1.0;
    size_t length = 129;
    double *column = data;
    eval_state_t reader = {.lengths = &length, .columns = &column};

    check(run_fold(&reader, -1, 1), "a failed allocation launches no worker and leaves the answer");
    check(run_fold(&reader, 0, 0), "a launch failing at the first worker joins none and fails");
    check(run_fold(&reader, 1, 0), "a launch failing at the second worker joins the one it started");
    check(run_fold(&reader, 2, 0), "a launch failing at the third worker joins the two it started");
    check(run_fold(&reader, -1, 0), "a full team of three recovers the fixed-tree sum");

    printf("\n%ld assertions, %ld failed\n", cases, failures);
    return failures
           ? RL_FAIL("replay-failure", cases, "replay-failure-not-recovered")
           : RL_PASS("replay-failure", cases);
}
