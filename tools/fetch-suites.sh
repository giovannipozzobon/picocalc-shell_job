#!/usr/bin/env bash
# fetch-suites.sh — copy the real UF2s the host tests read into
# test/suites/uf2/ (design.md §6), which is not committed. test_uf2 skips
# its real-file checks for any that is missing.
#
#   tools/fetch-suites.sh
#
# The sources are the owner's release builds of the three emulators and
# UF2 Loader's published output, through references/. Set PICO_ORIC,
# PICO_ATOM, PICO_ACE, LOADER_OUT to use others.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
refs="$root/references"
dest="$root/test/suites/uf2"
mkdir -p "$dest"

copy() {
    if [ -f "$1" ]; then
        cp "$1" "$dest/$2"
        echo "$2 <- $1"
    else
        echo "fetch-suites.sh: missing $1; $2 not fetched" >&2
    fi
}

loader="${LOADER_OUT:-$refs/uf2loader/output}"
copy "${PICO_ORIC:-$refs/pico-oric/build/pico-release/pico-oric.uf2}" pico-oric.uf2
copy "${PICO_ATOM:-$refs/pico-atom/build/v1.1-release/pico-atom.uf2}" pico-atom.uf2
copy "${PICO_ACE:-$refs/pico-jupiter-ace/build/pico-release/pico-ace.uf2}" pico-ace.uf2
copy "$loader/bootloader_pico2.uf2" stage3-pico2.uf2
copy "$loader/BOOT2350.uf2" loader-menu.uf2
copy "$loader/BOOT2040.uf2" loader-menu-rp2040.uf2
# This tree's own, which test_args reads for the record that says a program
# takes arguments: a real link's proof the record survives --gc-sections.
copy "$root/build/pico/args-test.uf2" args-test.uf2
copy "$root/build/pico/args-ram.uf2" args-ram.uf2
