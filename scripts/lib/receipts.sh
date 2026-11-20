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

# rl_make_work_directory creates a private scratch directory and prints its path.
# Arguments:
#   $1 - The prefix for the directory name, without a path
# Outputs:
#   The directory's path on stdout
# Returns:
#   0 when the directory was created, non-zero otherwise
rl_make_work_directory() {
    mktemp -d "${TMPDIR:-/tmp}/$1.XXXXXXXX"
}

# rl_status_field prints the value of one field of an RL-STATUS line.
# Arguments:
#   $1 - The field name, for example result, cases, skipped or reason
# Inputs:
#   The text to read, on stdin
# Outputs:
#   The field's value from the last receipt, empty if the field is missing or repeated
rl_status_field() {
    awk -v key="$1" '$1 == "RL-STATUS" {
        value = ""
        seen = 0
        for (i = 2; i <= NF; i++) {
            if (index($i, key "=") == 1) {
                if (++seen > 1) { value = ""; break }
                value = substr($i, length(key) + 2)
            }
        }
    }
    END { print value }'
}

# rl_report_row formats one leg's result for the run table.
# Globals:
#   RL_SORT_ROWS - Read
#   RL_ROW_BUFFER - Appended when RL_SORT_ROWS is 1
# Arguments:
#   $1 - The leg ID
#   $2 - PASS, SKIP or FAIL
#   $3 - The case count reported for the leg
#   $4 - The reason token, or a phrase where the row carries no token
# Outputs:
#   One fixed-width row on stdout when buffering is off
rl_report_row() {
    if [ "${RL_SORT_ROWS:-0}" = 1 ]; then
        RL_ROW_BUFFER="${RL_ROW_BUFFER:-}$(printf '%-4s %-4s %5s  %s' "$1" "$2" "$3" "$4")"$'\n'
    else
        printf '%-4s %-4s %5s  %s\n' "$1" "$2" "$3" "$4"
    fi
}

# rl_flush_rows prints the buffered rows in ID order, so a table whose legs run in dependency
# order still reads in numeric order. The sort key starts at the ID's second character, which is
# where the number begins.
# Globals:
#   RL_ROW_BUFFER - Read and cleared
# Outputs:
#   The buffered rows, sorted, on stdout
rl_flush_rows() {
    [ -n "${RL_ROW_BUFFER:-}" ] || return 0
    printf '%s' "$RL_ROW_BUFFER" | sort -k1.2n
    RL_ROW_BUFFER=""
}

# rl_floor_shortfall names a parse that read fewer rows than could mean anything, or prints nothing.
# Arguments:
#   $1 - What was parsed, phrased as it should read in the message
#   $2 - How many rows the parse produced
#   $3 - The smallest count that means the parse worked
# Outputs:
#   One space-prefixed message on stdout when the count is short, nothing otherwise
rl_floor_shortfall() {
    [ "$2" -lt "$3" ] || return 0
    printf ' %s: only %s parsed, expected at least %s' "$1" "$2" "$3"
}

# rl_in_set reports whether a word appears in a space-separated list.
# Arguments:
#   $1 - The word
#   $2 - The list, space separated
# Returns:
#   0 when the word is in the list, 1 otherwise
rl_in_set() {
    case " $2 " in
        *" $1 "*) return 0 ;;
    esac
    return 1
}
