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

// MAX_DIAGNOSTICS bounds the number of reports a sink retains. Further reports are
// dropped when the sink is full.
#define MAX_DIAGNOSTICS 64

// MESSAGE_MAX is the size in bytes of one formatted message, including the terminator. A
// longer message is truncated by vsnprintf rather than overrunning the entry.
#define MESSAGE_MAX 512

// QUOTED_LINE_MAX bounds the number of source bytes quoted around a diagnostic span.
#define QUOTED_LINE_MAX 512

// ENTRY_ERROR is the kind tag for an entry that is an error. Only these are counted by
// diagnostic_error_count.
#define ENTRY_ERROR 0

// ENTRY_NOTE marks an informational report. A note may accompany an error or stand alone.
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

// diag holds the source text and the reports recorded against it.
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

// diagnostic_note records an informational message at `at`.
//
// Takes sink (diagnostic_t) which is where diagnostics are collected.
// Takes at (span_t) which is the source position to blame.
// Takes fmt (char) which is the format string.
void diagnostic_note(diagnostic_t *sink, const span_t at, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    diagnostic_push(sink, ENTRY_NOTE, at, SPAN_NONE, 0, fmt, ap);
    va_end(ap);
}

// diagnostic_error_count counts the error entries retained by the sink.
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
// The tally is recomputed rather than decremented, because an entry may be a note and the
// two counts do not move together.
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
// Takes primary (span_t) which receives the location of the first error.
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

// diagnostic_line_start finds the first byte of a source line, including an empty final
// line.
//
// Takes source (char) which is the source text, borrowed.
// Takes wanted (int) which is the line number, counting from one.
// Returns a pointer into the source, or NULL if the line is absent.
static const char *diagnostic_line_start(const char *source, const int wanted) {
    if (!source || wanted < 1) {
        return NULL;
    }
    for (int line = 1; line < wanted; line++) {
        const char *newline = strchr(source, '\n');
        if (!newline) {
            return NULL;
        }
        source = newline + 1;
    }
    return source;
}

// diagnostic_excerpt_start chooses where to begin quoting a source line. It keeps the
// start of the span in view and leaves room for context on either side.
//
// Takes length (size_t) which is the length of the line in bytes.
// Takes column (size_t) which is the span's byte offset from the start of the line.
// Takes width (size_t) which is the number of source bytes to underline.
static size_t diagnostic_excerpt_start(const size_t length, const size_t column, const size_t width) {
    if (length <= QUOTED_LINE_MAX || column + width <= QUOTED_LINE_MAX) {
        return 0;
    }
    const size_t context = QUOTED_LINE_MAX / 2;
    const size_t start = column > context ? column - context : 0;
    return start < length - QUOTED_LINE_MAX ? start : length - QUOTED_LINE_MAX;
}

// diagnostic_render_marks writes the underline after the gutter, preserving tab stops in
// the source before the first marker.
//
// Takes stream (FILE) which receives the underline.
// Takes text (char) which points to the first source byte in the excerpt.
// Takes padding (size_t) which counts source bytes before the span.
// Takes width (size_t) which is the number of visible marker characters.
// Takes marker (char) which is the marker to repeat.
// Takes label (char) which is the annotation to append, if present.
static void diagnostic_render_marks(FILE *stream, const char *text, const size_t padding,
                                    const size_t width, const char *marker, const char *label) {
    for (size_t i = 0; i < padding; i++) {
        fputc(text[i] == '\t' ? '\t' : ' ', stream);
    }
    for (size_t i = 0; i < width; i++) {
        fputs(marker, stream);
    }
    if (label && *label) {
        fprintf(stream, " %s", label);
    }
    fputc('\n', stream);
}

// diagnostic_render_span quotes and underlines the source at a span.
//
// A long line is cropped around the span. The underline follows the same tabs as the
// excerpt and marks a missing token at the end of the line with one marker.
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
    const char *line = diagnostic_line_start(source, span.line);
    if (!line) {
        return;
    }
    size_t length = strcspn(line, "\n");
    if (length && line[length - 1] == '\r') {
        length--;
    }
    size_t column = span.column > 0 ? (size_t) span.column - 1 : 0;
    if (column > length) {
        column = length;
    }
    size_t width = span.length > 0 ? (size_t) span.length : 1;
    const size_t start = diagnostic_excerpt_start(length, column, width);
    const size_t end = length - start > QUOTED_LINE_MAX ? start + QUOTED_LINE_MAX : length;
    const int gutter = fprintf(stream, "  %4d", span.line);
    fputs(start ? " | ..." : " | ", stream);
    for (size_t i = start; i < end; i++) {
        fputc(line[i] == '\r' ? ' ' : (unsigned char) line[i], stream);
    }
    fputs(end < length ? "...\n" : "\n", stream);

    fprintf(stream, "%*s | ", gutter, "");
    if (start) {
        fputs("   ", stream);
    }
    if (width > end - column) {
        width = end > column ? end - column : 1;
    }
    diagnostic_render_marks(stream, line + start, column - start, width, marker, label);
}

// diagnostic_primary_label labels the demand site of a two-span error. Notes and
// single-span errors have an empty label.
//
// Takes entry (entry_t) which is the entry whose label is chosen.
static const char *diagnostic_primary_label(const entry_t *entry) {
    return entry->kind == ENTRY_ERROR && entry->secondary.line > 0
           ? "the demand is stated here"
           : "";
}

// diagnostic_render_entry prints the message and its primary span. For a two-span error
// it also prints the hop count and the span where the guarantee was lost.
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
