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

#include "diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MAX_DIAGNOSTICS is how many diagnostics one sink retains; reports past this limit are
// dropped, which bounds the sink when a caller keeps reporting in a loop.
#define MAX_DIAGNOSTICS 64

// MESSAGE_MAX is the size in bytes of one formatted message, including the terminator. A
// longer message is truncated by vsnprintf rather than overrunning the entry.
#define MESSAGE_MAX 512

// QUOTED_LINE_MAX is the longest source line the renderer will quote, in bytes including
// the terminator. A longer line is quoted up to this width and the caret row is
// unaffected, because the caret row is built from the span's own column rather than from
// the quoted text.
#define QUOTED_LINE_MAX 512

// ENTRY_ERROR is the kind tag for an entry that is an error. Only these are counted by
// diagnostic_error_count.
#define ENTRY_ERROR 0

// ENTRY_NOTE is the kind tag for an entry that is a note attached to the error before it.
#define ENTRY_NOTE 1

// entry_t is one recorded diagnostic.
typedef struct {
    // kind is ENTRY_ERROR or ENTRY_NOTE.
    int kind;

    // primary is where the report is anchored. For a lane violation this is the site of
    // the demand.
    span_t primary;

    // secondary is where the guarantee was lost. Its line is 0 when the report has only
    // one end.
    span_t secondary;

    // hops is the distance from the primary span to the secondary one along the blame
    // path, in steps.
    int hops;

    // message is the message, already expanded from its format string and arguments.
    char message[MESSAGE_MAX];
} entry_t;

// diag is a diagnostic sink: the source it reports against, and the reports made so far.
struct diag {
    // file is the name of the file being reported on, borrowed, or NULL to render as
    // "<input>".
    const char *file;

    // source is the whole source text, borrowed, used to quote the line a span points at.
    const char *source;

    // entries is the reports made so far, oldest first.
    entry_t entries[MAX_DIAGNOSTICS];

    // entry_count is how many leading elements of `entries` are in use.
    int entry_count;

    // error_count is how many of those entries are errors rather than notes.
    int error_count;
};

// diagnostic_new allocates an empty sink over `file` and `source`, both borrowed for the
// sink's lifetime.
//
// Takes file (char) which is the name of the file being reported on, borrowed.
// Takes source (char) which is the source text, borrowed.
diagnostic_t *diagnostic_new(const char *file, const char *source) {
    diagnostic_t *sink = (diagnostic_t *) calloc(1, sizeof *sink);
    if (!sink) {
        return NULL;
    }
    sink->file = file;
    sink->source = source;
    return sink;
}

// diagnostic_free releases a sink allocated by diagnostic_new.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
void diagnostic_free(diagnostic_t *sink) {
    free(sink);
}

// diagnostic_push appends one entry to the sink, formatting the message from `fmt` and
// the already-started argument list `ap`. Silently does nothing once the sink is full or
// when `sink` is NULL.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes kind (int) which is which sort of entry this is.
// Takes primary (span_t) which is the span the diagnostic points at.
// Takes secondary (span_t) which is the span that lost the guarantee.
// Takes hops (int) which is the distance between the two spans, in steps.
// Takes fmt (char) which is the format string.
// Takes ap (va_list) which is the variadic argument list.
static void diagnostic_push(
    diagnostic_t *sink,
    const int kind,
    const span_t primary,
    const span_t secondary,
    const int hops,
    const char *fmt,
    va_list ap
) {
    if (!sink || sink->entry_count >= MAX_DIAGNOSTICS) {
        return;
    }
    entry_t *entry = &sink->entries[sink->entry_count++];
    entry->kind = kind;
    entry->primary = primary;
    entry->secondary = secondary;
    entry->hops = hops;
    vsnprintf(entry->message, sizeof entry->message, fmt, ap);
    if (kind == ENTRY_ERROR) {
        sink->error_count++;
    }
}

// diagnostic_error reports an error at `at`.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes at (span_t) which is the source position to blame.
// Takes fmt (char) which is the format string.
void diagnostic_error(diagnostic_t *sink, const span_t at, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    diagnostic_push(sink, ENTRY_ERROR, at, SPAN_NONE, 0, fmt, ap);
    va_end(ap);
}

// diagnostic_error_with_cause reports an error at `primary` whose guarantee was lost at
// `secondary`, `hops` steps upstream.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes primary (span_t) which is the span the diagnostic points at.
// Takes secondary (span_t) which is the span that lost the guarantee.
// Takes hops (int) which is the distance between the two spans, in steps.
// Takes fmt (char) which is the format string.
void diagnostic_error_with_cause(diagnostic_t *sink, const span_t primary, const span_t secondary,
                                 const int hops, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    diagnostic_push(sink, ENTRY_ERROR, primary, secondary, hops, fmt, ap);
    va_end(ap);
}

// diagnostic_note reports a note at `at`, attached to the error before it.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes at (span_t) which is the source position to blame.
// Takes fmt (char) which is the format string.
void diagnostic_note(diagnostic_t *sink, const span_t at, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    diagnostic_push(sink, ENTRY_NOTE, at, SPAN_NONE, 0, fmt, ap);
    va_end(ap);
}

// diagnostic_error_count is how many errors the sink holds; notes are not counted.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
int diagnostic_error_count(const diagnostic_t *sink) {
    return sink ? sink->error_count : 0;
}

// diagnostic_mark is how many entries the sink holds.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Returns the entry count.
int diagnostic_mark(const diagnostic_t *sink) {
    return sink ? sink->entry_count : 0;
}

// diagnostic_rewind discards every entry recorded after `mark`.
//
// Recomputing the tally rather than decrementing it matters because an entry may be a
// note and the two counts do not move together.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes mark (int) which is a value diagnostic_mark returned earlier for this sink.
void diagnostic_rewind(diagnostic_t *sink, const int mark) {
    if (!sink || mark < 0 || mark >= sink->entry_count) {
        return;
    }
    sink->entry_count = mark;
    sink->error_count = 0;
    for (int i = 0; i < sink->entry_count; i++) {
        if (sink->entries[i].kind == ENTRY_ERROR) {
            sink->error_count++;
        }
    }
}

// diagnostic_first_spans fetches the spans and hop count of the sink's first error, when
// the sink holds one.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes primary (span_t) which receives the demand site.
// Takes secondary (span_t) which receives the span that lost the guarantee.
// Takes hops (int) which receives the hop count between the two.
int diagnostic_first_spans(const diagnostic_t *sink, span_t *primary, span_t *secondary, int *hops) {
    if (!sink) {
        return 0;
    }
    for (int i = 0; i < sink->entry_count; i++) {
        if (sink->entries[i].kind != ENTRY_ERROR) {
            continue;
        }
        if (primary) {
            *primary = sink->entries[i].primary;
        }
        if (secondary) {
            *secondary = sink->entries[i].secondary;
        }
        if (hops) {
            *hops = sink->entries[i].hops;
        }
        return 1;
    }
    return 0;
}

// diagnostic_line_text copies the 1-based line `wanted` out of `source` into `buffer`,
// which holds `capacity` bytes including the terminator.
//
// Takes source (char) which is the source text, borrowed.
// Takes wanted (int) which is the 1-based line to copy.
// Takes buffer (char) which receives the bytes.
// Takes capacity (size_t) which bounds how many entries may be written.
static int diagnostic_line_text(const char *source, const int wanted, char *buffer, const size_t capacity) {
    if (!source || wanted < 1) {
        return -1;
    }
    int line = 1;
    const char *cursor = source;
    while (line < wanted && *cursor) {
        if (*cursor == '\n') {
            line++;
        }
        cursor++;
    }
    if (line != wanted) {
        return -1;
    }
    const char *end = cursor;
    while (*end && *end != '\n') {
        end++;
    }
    size_t length = (size_t) (end - cursor);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(buffer, cursor, length);
    buffer[length] = '\0';
    return (int) length;
}

// diagnostic_render_markers writes the caret row that sits under a quoted source line:
// leading blanks up to the span's column, then `marker` repeated across the span's width,
// then `label` if there is one.
//
// Takes stream (FILE) which is where the output is written.
// Takes span (span_t) which is the source position to blame.
// Takes marker (char) which is the marker repeated across the span's width.
// Takes label (char) which is the annotation written after the underline, or empty.
static void diagnostic_render_markers(FILE *stream, const span_t span, const char *marker, const char *label) {
    fprintf(stream, "       | ");
    for (int i = 1; i < span.column; i++) {
        fputc(' ', stream);
    }
    const int marker_width = span.length > 0 ? span.length : 1;
    for (int i = 0; i < marker_width; i++) {
        fputs(marker, stream);
    }
    if (label && *label) {
        fprintf(stream, " %s", label);
    }
    fputc('\n', stream);
}

// diagnostic_render_span quotes the source line a span lies on and underlines the span
// itself with `marker`, annotating the underline with `label`. Renders nothing when the
// span is absent or its line is not in the source.
//
// Takes stream (FILE) which is where the output is written.
// Takes source (char) which is the source text, borrowed.
// Takes span (span_t) which is the source position to blame.
// Takes marker (char) which is the marker repeated across the span's width.
// Takes label (char) which is the annotation written after the underline, or empty.
static void diagnostic_render_span(
    FILE *stream,
    const char *source,
    const span_t span,
    const char *marker,
    const char *label
) {
    if (span.line <= 0) {
        return;
    }
    char line_buffer[QUOTED_LINE_MAX];
    if (diagnostic_line_text(source, span.line, line_buffer, sizeof line_buffer) < 0) {
        return;
    }
    fprintf(stream, "  %4d | %s\n", span.line, line_buffer);
    diagnostic_render_markers(stream, span, marker, label);
}

// diagnostic_primary_label chooses the label for an entry's primary underline. Only an
// error's primary span is the demand site, and only when the entry has a second end; a
// note points at whatever it is about, so labelling it "the demand is stated here" would
// be a lie.
//
// Takes entry (entry_t) which is the entry whose label is chosen.
static const char *diagnostic_primary_label(const entry_t *entry) {
    return entry->kind == ENTRY_ERROR && entry->secondary.line > 0
           ? "the demand is stated here"
           : "";
}

// diagnostic_render_entry renders one entry: the message line, the primary span, and,
// when the entry has a second end, the hop count and the span that lost the guarantee.
//
// Takes stream (FILE) which is where the output is written.
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes entry (entry_t) which is the entry being rendered.
static void diagnostic_render_entry(FILE *stream, const diagnostic_t *sink, const entry_t *entry) {
    const char *kind = entry->kind == ENTRY_ERROR ? "error" : "note";
    fprintf(stream, "%s:%d:%d: %s: %s\n",
            sink->file ? sink->file : "<input>",
            entry->primary.line, entry->primary.column, kind, entry->message);
    diagnostic_render_span(stream, sink->source, entry->primary, "^", diagnostic_primary_label(entry));
    if (entry->secondary.line <= 0) {
        return;
    }
    fprintf(stream, "  ... the guarantee was lost %d step%s upstream:\n",
            entry->hops, entry->hops == 1 ? "" : "s");
    diagnostic_render_span(stream, sink->source, entry->secondary, "~", "introduced here");
}

// diagnostic_render renders every entry to `stream`, oldest first.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes stream (void) which is where the output is written.
void diagnostic_render(const diagnostic_t *sink, void *stream) {
    FILE *out = (FILE *) stream;
    if (!sink) {
        return;
    }
    for (int i = 0; i < sink->entry_count; i++) {
        diagnostic_render_entry(out, sink, &sink->entries[i]);
    }
}

// diagnostic_message_contains reports whether any message in the sink holds `needle`.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes needle (char) which is the text to look for.
// Returns 1 when some message holds it, 0 otherwise.
int diagnostic_message_contains(const diagnostic_t *sink, const char *needle) {
    if (!sink || !needle) {
        return 0;
    }
    for (int i = 0; i < sink->entry_count; i++) {
        if (strstr(sink->entries[i].message, needle)) {
            return 1;
        }
    }
    return 0;
}
