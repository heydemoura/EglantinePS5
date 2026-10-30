#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copies the Eglantine submodule (the emulator source) into a build cache's source tree:
#   sync-eden-source.sh <source tree>
# Only what changed is copied, so the tree's package cache (.cache) and the objects built from it
# stay. The tree's .unpacked-from names the submodule's commit, plus a hash of any local changes
# in it; a tree that already carries that stamp is left alone.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tree=$1
src="$root/Eglantine"
[[ -f $src/CMakeLists.txt ]] || { echo "The Eglantine submodule is empty: git submodule update --init Eglantine" >&2; exit 1; }

commit=$(git -C "$src" rev-parse HEAD)
stamp=eglantine-$commit
if [[ -n $(git -C "$src" status --porcelain --untracked-files=all) ]]; then
    stamp+=-$( { git -C "$src" diff HEAD; git -C "$src" ls-files --others --exclude-standard -z | xargs -0 -r sha256sum; } | sha256sum | cut -c1-12)
fi

if [[ ! -f $tree/CMakeLists.txt || $(cat "$tree/.unpacked-from" 2>/dev/null) != "$stamp" ]]; then
    echo "== Eden source ($stamp)"
    mkdir -p "$tree"
    rsync -a --delete \
        --exclude=/.git --exclude=/.cache --exclude=/.unpacked-from --exclude=/GIT-COMMIT --exclude=/GIT-REFSPEC \
        "$src"/ "$tree"/
    printf '%s\n' "$stamp" > "$tree/.unpacked-from"
fi
printf '%s\n' "$commit" > "$tree/GIT-COMMIT"
printf '%s\n' 'ps5-headless' > "$tree/GIT-REFSPEC"
