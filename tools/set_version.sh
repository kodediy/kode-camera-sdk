#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Moves the SDK to one version for a release, the way kode-sdk's tool of the
# same name does: every component's own `version: "X"`, every kodediy/camera_*
# dependency's `"==X"` in the manifests, the version the code says
# (CAMERA_SDK_VERSION, CAMERA_AOS_SDK_VERSION) and the tag the README tells an
# app to pin kodediy/camera at (`version: vX`). The kode-sdk pin is left as it
# is: it moves on its own. tools/check_example_pins.sh checks that they agree.
#
#   usage: tools/set_version.sh <X.Y.Z>
#   exit:  0 done, 2 not a version
#
set -euo pipefail
cd "$(dirname "$0")/.."

v="${1:-}"
if ! [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: tools/set_version.sh <X.Y.Z>" >&2
    exit 2
fi

for f in components/*/idf_component.yml; do
    sed -i.bak -E \
        -e "s/^version: \"[0-9.]+\"/version: \"$v\"/" \
        -e "/kodediy\/camera_[a-z_]+:/ s/\"==[0-9.]+\"/\"==$v\"/" \
        "$f"
    rm -f "$f.bak"
done
sed -i.bak -E "s/^(#define CAMERA_SDK_VERSION )\"[0-9.]+\"/\1\"$v\"/" components/camera_types/include/camera_types.h
sed -i.bak -E "s/^(#define CAMERA_AOS_SDK_VERSION )\"[0-9.]+\"/\1\"$v\"/" components/camera_aos/core/include/camera_aos.h
# In the README, only the version under the kode-camera-sdk git line.
sed -i.bak -E "/kode-camera-sdk\.git/{n;n;s/^( +version: )v[0-9.]+/\1v$v/;}" README.md
rm -f components/camera_types/include/camera_types.h.bak components/camera_aos/core/include/camera_aos.h.bak README.md.bak
tools/check_example_pins.sh
