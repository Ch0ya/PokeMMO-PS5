#!/usr/bin/env bash
# Copy the built image to a release-style name and record it for the staging auto-updater:
#   PokeMMO-PPSA98001-v<client version>-Rev<loader revision>-<build commit>.ffpfsc
# The title ID inside the image is what the console uses; the file name is for humans and the updater.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
image=dist/PPSA98001.ffpfsc
header=build/titles/PPSA98001/src/pokemmo_version.h
[[ -s $image && -s $header ]] || { echo "build the image first (make image-loader)" >&2; exit 2; }
release=$(sed -n 's/#define POKEMMO_RELEASE "\(.*\)"/\1/p' "$header")
revision=$(sed -n 's/#define POKEMMO_REVISION "\(.*\)"/\1/p' "$header")
version=$(sed -n 's/#define POKEMMO_VERSION "\(.*\)"/\1/p' "$header")
commit=$(printf '%s' "$version" | sed -n 's/.*-g\([0-9a-f]\{7,\}\).*/\1/p')
[ -n "$commit" ] || commit=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)
name="PokeMMO-PPSA98001-v${release}-Rev${revision}-${commit}.ffpfsc"
cp -f "$image" "dist/$name"
printf '%s\n' "$name" > dist/latest.txt
printf '%s\n' "dist/$name"
sha256sum "dist/$name"
