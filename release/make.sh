#!/bin/sh
# Make a test release, so the tag baked into xlive.dll (XLS_RELEASE_TAG, what the self-updater compares with
# GitHub's latest release) always matches the release it ships in.
#
#   release/make.sh                      build dist/lp2-recolonised-test.zip with the next tag
#   release/make.sh --publish NOTES.md   ...and create the GitHub release (clean, pushed tree only)
#
# Next tag: test-YYYY-MM-DD, then test-YYYY-MM-DDb, c, ... for more releases the same day.
# BUILD (default ../lp2-build/mingw next to the repo) is the configured CMake build folder.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$REPO/../lp2-build/mingw}
GH_REPO=FlawedOberron/xlive-steamworks-lp2
PUBLISH=
if [ "${1:-}" = "--publish" ]; then
	PUBLISH=${2:?release notes file}
	[ -f "$PUBLISH" ] || { echo "no notes file $PUBLISH"; exit 1; }
	if [ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ] || [ -n "$(git -C "$REPO" log '@{u}..' --oneline)" ]; then
		echo "commit and push first: the release tags the pushed branch"; exit 1
	fi
fi

DATE=$(date +%F)
TAGS=$(gh release list -R "$GH_REPO" -L 100 --json tagName -q '.[].tagName')
TAG=test-$DATE
for s in b c d e f g h i j k l m n o p; do
	echo "$TAGS" | grep -qx "$TAG" || break
	TAG=test-$DATE$s
done
echo "$TAGS" | grep -qx "$TAG" && { echo "out of tags for $DATE"; exit 1; }

cmake -S "$REPO" -B "$BUILD" -DXLS_RELEASE_TAG="$TAG" >/dev/null
cmake --build "$BUILD" -j16
grep -aq "$TAG" "$REPO/bin/xlive.dll" || { echo "xlive.dll doesn't carry $TAG"; exit 1; }

OUT=$REPO/dist
STAGE=$OUT/stage
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp "$REPO/bin/xlive.dll" "$REPO/bin/steam_api.dll" "$REPO/release/xlive_steamworks.json" "$STAGE/"
sed "s/@DATE@/${TAG#test-}/" "$REPO/release/README.txt" > "$STAGE/README.txt"
rm -f "$OUT/lp2-recolonised-test.zip"
(cd "$STAGE" && zip -q9 "$OUT/lp2-recolonised-test.zip" README.txt steam_api.dll xlive.dll xlive_steamworks.json)
rm -rf "$STAGE"
echo "$TAG -> $OUT/lp2-recolonised-test.zip"

if [ -n "$PUBLISH" ]; then
	gh release create "$TAG" "$OUT/lp2-recolonised-test.zip" -R "$GH_REPO" --target "$(git -C "$REPO" rev-parse HEAD)" \
		--title "Lost Planet 2 (Recolonised) test build ${TAG#test-}" --notes-file "$PUBLISH" --latest
fi
