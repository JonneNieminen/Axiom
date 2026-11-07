#!/usr/bin/env bash
# Builds a drag-to-install DMG from a built Axiom.app.
# The DMG holds Axiom.app, an Applications shortcut and "READ ME FIRST.txt" with the
# xattr command to run an unnotarized build.
#
# Usage: build/macos/make-dmg.sh <Axiom.app> <version> <arch> <output-dir>
set -euo pipefail

src="$1"; version="$2"; arch="$3"; out="$4"
here="$(cd "$(dirname "$0")" && pwd)"
case "$arch" in
  arm64)  label="Apple silicon" ;;
  x86_64) label="Intel" ;;
  *)      label="$arch" ;;
esac
[[ -d "$src" ]] || { echo "No app at $src" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# ditto keeps the bundle's symlinks, permissions and code signature intact.
ditto "$src" "$stage/Axiom.app"
if ! codesign --verify --deep --strict "$stage/Axiom.app" 2>/dev/null; then
  echo "Bundle signature doesn't verify; applying an ad-hoc signature."
  codesign --force --deep --sign - "$stage/Axiom.app"
  codesign --verify --deep --strict "$stage/Axiom.app"
fi

ln -s /Applications "$stage/Applications"
sed -e "s/{{VERSION}}/$version/g" -e "s/{{ARCH}}/$label/g" \
  "$here/READ ME FIRST.txt" > "$stage/READ ME FIRST.txt"

mkdir -p "$out"
dmg="$out/Axiom-$version-macos-$arch.dmg"
rm -f "$dmg"
hdiutil create -volname "Axiom $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg" >/dev/null
echo "Created $dmg"
