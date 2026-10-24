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

#ifndef LANELANG_DIAGNOSTIC_H
#define LANELANG_DIAGNOSTIC_H

#include <stddef.h>

// span_t is a run of source bytes on a single line, so a diagnostic can point at the text
// it is about.
typedef struct {
    // file is the name of the file the span lies in, borrowed from the caller, or NULL
    // when unnamed.
    const char *file;

    // line is the line the span lies on, counted from 1.
    int line;

    // column is the byte offset of the span's first character within that line, counted
    // from 1.
    int column;

    // length is the width of the span in bytes. The renderer clamps it to at least 1, so
    // a position with no width still draws a single caret.
    int length;
} span_t;

// SPAN_NONE is the span used where there is no position to report. A line of 0 marks it
// absent, and every reader tests that field rather than comparing whole spans.
static const span_t SPAN_NONE = {0, 0, 0, 0};

// diagnostic_t is a sink holding the diagnostics reported against one source text.
typedef struct diag diagnostic_t;

// diagnostic_new creates a diagnostic sink over the source text. Both `file` and `source`
// are borrowed and must outlive the sink. Returns NULL if allocation fails.
//
// Takes file (char) which is the name of the file being reported on, borrowed.
// Takes source (char) which is the source text, borrowed.
diagnostic_t *diagnostic_new(const char *file, const char *source);

// diagnostic_free releases a sink created by diagnostic_new. Passing NULL is allowed and
// does nothing.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
void diagnostic_free(diagnostic_t *sink);

// diagnostic_error records a one-span error, which is a parse or resolution failure.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes at (span_t) which is the source position to blame.
// Takes fmt (char) which is the format string.
void diagnostic_error(diagnostic_t *sink, span_t at, const char *fmt, ...);

// diagnostic_error_with_cause records a two-span error, which is the lane violation.
// `primary` is the demand site, `secondary` the node that introduced the weaker class,
// and `hops` the distance between the two along the blame path.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes primary (span_t) which is the span the diagnostic points at.
// Takes secondary (span_t) which is the span that lost the guarantee.
// Takes hops (int) which is the distance between the two spans, in steps.
// Takes fmt (char) which is the format string.
void diagnostic_error_with_cause(
    diagnostic_t *sink,
    span_t primary,
    span_t secondary,
    int hops,
    const char *fmt,
    ...
);

// diagnostic_note records a note attached to the error before it. A note is not counted
// as an error.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes at (span_t) which is the source position to blame.
// Takes fmt (char) which is the format string.
void diagnostic_note(diagnostic_t *sink, span_t at, const char *fmt, ...);

// diagnostic_error_count reports how many errors have been recorded, ignoring notes. A
// NULL sink has none.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
int diagnostic_error_count(const diagnostic_t *sink);

// diagnostic_mark records how much the sink holds, and diagnostic_rewind discards
// everything recorded since a mark. A NULL sink marks at 0 and rewinds to nothing.
//
// The pair exists only for speculative reads. In a build that leaves the lane qualifiers
// unreserved, the parser tells a qualifier from a name by one token of lookahead with a
// save-and-restore of its own cursor, and restoring the cursor alone puts the tokens back
// but not the diagnostics: a lexer error inside the lookahead is recorded once there and
// once again when the caller re-reads the same character, so a program with one bad byte
// reports it twice. The cursor and the sink
// rewind together or neither does. Rewinding past a mark taken before an error the caller
// means to keep would discard it, so a mark is taken and used in one place, immediately
// around the speculative read.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Returns how many entries the sink holds.
int diagnostic_mark(const diagnostic_t *sink);

// diagnostic_rewind discards every entry recorded after `mark`.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes mark (int) which is a value diagnostic_mark returned earlier for this sink.
void diagnostic_rewind(diagnostic_t *sink, int mark);

// diagnostic_render renders every recorded diagnostic to `stream`, with source excerpts
// and carets.
//
// `stream` is a `FILE *`, typed as `void *` so this header does not drag <stdio.h> into
// every translation unit that only wants to report an error.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes stream (void) which is where the output is written.
void diagnostic_render(const diagnostic_t *sink, void *stream);

// diagnostic_first_spans reports the spans of the first recorded error, for the fixture
// runner's EXPECT lines.
//
// Writes the demand site through `primary`, the site that lost the guarantee through
// `secondary`, and the distance between them through `hops`; any of the three may be
// NULL. Returns 1 when an error was found and 0 when the sink holds none.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes primary (span_t) which receives the demand site.
// Takes secondary (span_t) which receives the span that lost the guarantee.
// Takes hops (int) which receives the hop count between the two.
int diagnostic_first_spans(const diagnostic_t *sink, span_t *primary, span_t *secondary, int *hops);

// diagnostic_message_contains reports whether any message in the sink holds `needle`.
//
// It exists so a fixture can assert what a refusal says and not only that one happened.
// The match is a plain substring over the expanded message, so a fixture quotes the
// clause it cares about, and rewording the rest of a message does not fail a fixture that
// does not quote it.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes needle (char) which is the text to look for.
// Returns 1 when some message holds it, 0 otherwise.
int diagnostic_message_contains(const diagnostic_t *sink, const char *needle);

#endif  // LANELANG_DIAGNOSTIC_H
