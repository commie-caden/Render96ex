#!/usr/bin/env bash
# Verify that every shipped file extracted intact.
#
# Run from the rt64-vk directory:  ./verify_install.sh
# Checks names and contents, so a truncated or skipped file is caught.

set -uo pipefail
cd "$(dirname "$0")" || exit 1

if [ ! -f MANIFEST.sha256 ]; then
    echo "MANIFEST.sha256 missing — the extraction dropped it too."
    echo "Re-extract the archive over the game source root."
    exit 1
fi

missing=0
corrupt=0
while read -r sum path; do
    [ -z "$path" ] && continue
    if [ ! -f "$path" ]; then
        printf '  MISSING  %s\n' "$path"
        missing=$((missing+1))
    elif [ "$(sha256sum "$path" | cut -d' ' -f1)" != "$sum" ]; then
        printf '  CORRUPT  %s\n' "$path"
        corrupt=$((corrupt+1))
    fi
done < MANIFEST.sha256

total=$(grep -c . MANIFEST.sha256)
if [ "$missing" -eq 0 ] && [ "$corrupt" -eq 0 ]; then
    echo "  all $total files present and intact"
    exit 0
fi
echo
echo "  $missing missing, $corrupt corrupt, out of $total"
echo "  Re-extract the archive over the game source root:"
echo "    tar -xJf rt64-vk-*.tar.xz"
exit 1
