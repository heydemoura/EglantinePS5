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
    stamp+=-$( ( cd "$src"; git diff HEAD; git ls-files --others --exclude-standard -z | xargs -0 -r sha256sum ) | sha256sum | cut -c1-12)
fi

# rsync -a preserves old timestamps. Restoring an older source revision can then
# leave a newer Ninja object untouched even though its contents no longer match.
# Copy changed contents with a fresh destination timestamp; refresh legacy mirrors
# once so previously missed rebuilds cannot survive this policy transition.
sync_policy=content-mtime-v1
policy_changed=0
[[ $(cat "$tree/.eden-sync-policy" 2>/dev/null) == "$sync_policy" ]] || policy_changed=1
if [[ ! -f $tree/CMakeLists.txt || $(cat "$tree/.unpacked-from" 2>/dev/null) != "$stamp" || $policy_changed == 1 ]]; then
    echo "== Eden source ($stamp)"
    mkdir -p "$tree"
    comparison=(--checksum)
    if [[ $policy_changed == 1 ]]; then comparison=(--ignore-times); fi
    rsync -a --no-times "${comparison[@]}" --delete \
        --exclude=/.git --exclude=/.cache --exclude=/.eden-sync-policy --exclude=/.unpacked-from --exclude=/GIT-COMMIT --exclude=/GIT-REFSPEC \
        "$src"/ "$tree"/
    printf '%s\n' "$stamp" > "$tree/.unpacked-from"
    printf '%s\n' "$sync_policy" > "$tree/.eden-sync-policy"
fi
printf '%s\n' "$commit" > "$tree/GIT-COMMIT"
printf '%s\n' 'ps5-headless' > "$tree/GIT-REFSPEC"
