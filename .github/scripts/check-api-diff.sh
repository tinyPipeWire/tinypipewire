#!/usr/bin/env bash
# Warns (does not fail the build) when a public function in include/tpw/*.h
# is removed or has its signature changed compared to the base branch.
# Every public header is copied and preprocessed, so a header that another
# one includes must be listed too; add new public headers to PUBLIC_HEADERS.
#
# Usage: check-api-diff.sh <base_ref> [summary_file]
# Writes a markdown bullet list of changes to summary_file (if any), and
# a `changed=true|false` line to $GITHUB_OUTPUT when run as a GitHub
# Actions step with an `id:` set.

set -u

base_ref="$1"
summary_file="${2:-}"
base_dir="$(mktemp -d)/tpw"
mkdir -p "$base_dir"

PUBLIC_HEADERS="tpw_export.h tpw_stream.h tpw_filter.h tpw_log.h"

for f in $PUBLIC_HEADERS; do
    git show "$base_ref:include/tpw/$f" > "$base_dir/$f" 2>/dev/null
done

extract_signatures() {
    local include_dir="$1"
    for header in $PUBLIC_HEADERS; do
        gcc -E -P -DTPW_API= -I "$include_dir" "$include_dir/tpw/$header" 2>/dev/null
    done \
        | tr '\n' ' ' | tr -s ' ' \
        | sed 's/;/;\n/g' \
        | grep -E 'tpw_[A-Za-z0-9_]+ *\(' \
        | grep -vE '\(\*' \
        | grep -v '^ *typedef' \
        | sed 's/^ *//; s/ *$//'
}

func_name() {
    grep -oP 'tpw_[A-Za-z0-9_]+(?=\()' <<< "$1" | head -1
}

declare -A base_sig head_sig

while IFS= read -r line; do
    [ -z "$line" ] && continue
    name="$(func_name "$line")"
    [ -n "$name" ] && base_sig["$name"]="$line"
done <<< "$(extract_signatures "$(dirname "$base_dir")")"

while IFS= read -r line; do
    [ -z "$line" ] && continue
    name="$(func_name "$line")"
    [ -n "$name" ] && head_sig["$name"]="$line"
done <<< "$(extract_signatures include)"

changed=0
summary=""
for name in "${!base_sig[@]}"; do
    if [ -z "${head_sig[$name]+x}" ]; then
        echo "::warning::Public API function removed: ${name}()"
        summary="${summary}- \`${name}()\` was removed
"
        changed=1
    elif [ "${head_sig[$name]}" != "${base_sig[$name]}" ]; then
        echo "::warning::Public API signature changed: ${name}()"
        summary="${summary}- \`${name}()\` signature changed
  - before: \`${base_sig[$name]}\`
  - after: \`${head_sig[$name]}\`
"
        changed=1
    fi
done

if [ "$changed" -eq 1 ]; then
    echo "Public API changes detected vs $base_ref (warning only, does not fail the build)."
    [ -n "$summary_file" ] && printf '%s' "$summary" > "$summary_file"
else
    echo "No public API signature changes detected."
fi

if [ -n "${GITHUB_OUTPUT:-}" ]; then
    if [ "$changed" -eq 1 ]; then
        echo "changed=true" >> "$GITHUB_OUTPUT"
    else
        echo "changed=false" >> "$GITHUB_OUTPUT"
    fi
fi

exit 0
