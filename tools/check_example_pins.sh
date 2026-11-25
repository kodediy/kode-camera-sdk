#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The kode-sdk version the projects pin, checked to be one, and this SDK's own.
#
# An example and the target build are each compiled against the
# kode-sdk commit (or tag) their own main/idf_component.yml names, not against
# the tree they sit in, and the README shows the same pin for someone to copy
# into an app of their own. When the pin moves in some of those and not in the
# others, the projects stop being the same test: the target builds on one
# kode-sdk and the examples on another, and what the README tells an app
# to use is neither. Bumping the pin is supposed to be a change to every one of
# them at once, and this makes that something CI asks for rather than
# something a commit remembers. (kode-sdk's own check of the same name also
# asks that the pin is not older than the newest release; that needs
# kode-sdk's tags, and this is another repository, so it asks less.)
#
# Three rules:
#   1. Every pin of kode-sdk says the same version. The pins are the
#      `version:` under `kodediy/kode` in every example, in tests/target and
#      in the README's yaml block, and the KODE_SDK_VERSION that the host
#      suites clone (tests/kode_sdk.cmake, tests/host_ao, tests/host_sys).
#   2. That version is a pin: a 40-digit commit, or a release tag `vXX.XX.XX`.
#      A branch name is a pin to whatever it points at next.
#   3. This SDK's own components say one version (`version: "X"` in each
#      components/*/idf_component.yml), the code says the same
#      (CAMERA_SDK_VERSION in camera_types.h, CAMERA_AOS_SDK_VERSION in
#      camera_aos.h, which goes into every photo's EXIF), and the README tells
#      an app to pin kodediy/camera at that release, `vX`.
#
# Today the pin is kode-sdk's release v0.1.5 (API v4).
#
#   usage: tools/check_example_pins.sh
#   exit:  0 the pins agree and are pins, 1 otherwise
#
set -uo pipefail
cd "$(dirname "$0")/.."

# Every project that pins kode-sdk, found rather than listed, so a new one is
# checked the day it lands.
FILES=( examples/*/main/idf_component.yml
        tests/*/main/idf_component.yml
        README.md )

pins=""
for f in "${FILES[@]}"; do
    if [ ! -f "$f" ]; then
        printf '\033[31mFAIL\033[0m  %s: not there\n' "$f"
        exit 1
    fi
    # The pin is the first `version:` line inside the dependency's own block:
    # the lines indented deeper than the `kodediy/kode:` key, up to the next
    # key at its level. A file also holds `kodediy/camera` and the others, whose
    # versions are not this one, and comments, which are skipped. The README
    # holds prose around its block, which never has the key on a line of its
    # own.
    v=$(awk '
        /^[[:space:]]*#/ { next }
        /^[[:space:]]*$/ { next }
        {
            match($0, /^[[:space:]]*/); ind = RLENGTH
            if ($0 ~ /^[[:space:]]+kodediy\/(kode|kdefs):[[:space:]]*$/) {
                inside = 1; key = ind; next
            }
            if (inside && ind <= key) { inside = 0 }
            if (inside && $0 ~ /^[[:space:]]+version:/) {
                line = $0
                sub(/^[[:space:]]+version:[[:space:]]*/, "", line)
                sub(/[[:space:]]*#.*$/, "", line)
                gsub(/["\047]/, "", line)
                sub(/[[:space:]]+$/, "", line)
                print line
                inside = 0
            }
        }' "$f")
    if [ -z "$v" ]; then
        printf '\033[31mFAIL\033[0m  %s: no `version:` under kodediy/kode (or kodediy/kdefs)\n' "$f"
        exit 1
    fi
    printf '      %-44s %s\n' "$f" "$v"
    pins="$pins$v"$'\n'
done

# The host suites clone kode-sdk by tag, outside the component manager.
for f in tests/kode_sdk.cmake tests/host_ao/CMakeLists.txt tests/host_sys/CMakeLists.txt; do
    if [ ! -f "$f" ]; then
        printf '\033[31mFAIL\033[0m  %s: not there\n' "$f"
        exit 1
    fi
    v=$(sed -n 's/^set(KODE_SDK_VERSION "\([^"]*\)")$/\1/p' "$f")
    if [ -z "$v" ]; then
        printf '\033[31mFAIL\033[0m  %s: no set(KODE_SDK_VERSION "...")\n' "$f"
        exit 1
    fi
    printf '      %-44s %s\n' "$f" "$v"
    pins="$pins$v"$'\n'
done

distinct=$(printf '%s' "$pins" | sort -u)
if [ -z "$distinct" ]; then
    printf '\033[31mFAIL\033[0m  no pin of kode-sdk found anywhere\n'
    exit 1
fi
if [ "$(printf '%s\n' "$distinct" | wc -l)" -ne 1 ]; then
    printf '\033[31mFAIL\033[0m  the pins disagree; one kode-sdk, one version:\n'
    printf '%s\n' "$distinct" | sed 's/^/      /'
    exit 1
fi
pin="$distinct"

if ! printf '%s' "$pin" | grep -Eq '^([0-9a-f]{40}|v[0-9]+\.[0-9]+\.[0-9]+)$'; then
    printf '\033[31mFAIL\033[0m  %s is not a pin: name a 40-digit commit or a release tag vXX.XX.XX\n' "$pin"
    exit 1
fi

printf '\033[32mok\033[0m    every project pins kode-sdk at %s\n' "$pin"

# Rule 3: one version for this SDK's components, and the README's pin of it.
own=""
for f in components/*/idf_component.yml; do
    v=$(sed -n 's/^version: "\([0-9][0-9.]*\)"$/\1/p' "$f")
    if [ -z "$v" ]; then
        printf '\033[31mFAIL\033[0m  %s: no `version: "X"`\n' "$f"
        exit 1
    fi
    own="$own$v"$'\n'
done
own=$(printf '%s' "$own" | sort -u)
if [ "$(printf '%s\n' "$own" | wc -l)" -ne 1 ]; then
    printf '\033[31mFAIL\033[0m  the components disagree on their version:\n'
    printf '%s\n' "$own" | sed 's/^/      /'
    exit 1
fi
for pair in "components/camera_types/include/camera_types.h CAMERA_SDK_VERSION" \
            "components/camera_aos/core/include/camera_aos.h CAMERA_AOS_SDK_VERSION"; do
    f=${pair% *}; name=${pair#* }
    v=$(sed -n "s/^#define $name \"\([0-9.]*\)\".*/\1/p" "$f")
    if [ "$v" != "$own" ]; then
        printf '\033[31mFAIL\033[0m  %s: %s is "%s"; the components are %s\n' "$f" "$name" "$v" "$own"
        exit 1
    fi
done
readme=$(awk '
    /^[[:space:]]+kodediy\/camera(_sys)?:/ { inside = 1; next }
    inside && /^[[:space:]]+version:/ {
        line = $0; sub(/^[[:space:]]+version:[[:space:]]*/, "", line); sub(/[[:space:]]*#.*$/, "", line)
        print line; exit
    }' README.md)
if [ "v$own" != "$readme" ]; then
    printf '\033[31mFAIL\033[0m  the components are %s and the README pins kodediy/camera at %s\n' "$own" "${readme:-nothing}"
    exit 1
fi
printf '\033[32mok\033[0m    the components and the code are %s, and the README pins v%s\n' "$own" "$own"
exit 0
