#!/usr/bin/env bash
# Downloads pinned third-party dependencies listed in third_party/deps.json.
# Every download is verified against its SHA-256 before it is used; a mismatch aborts.
# Re-running is cheap: a dependency whose stamp matches its pinned hash is skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="$ROOT/third_party"
MANIFEST="$THIRD_PARTY/deps.json"
DOWNLOADS="$THIRD_PARTY/.downloads"
mkdir -p "$DOWNLOADS"

need() { command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' is required" >&2; exit 1; }; }
need curl
need python3
need unzip

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# Destinations are deleted before being replaced, so refuse anything that is not
# strictly inside third_party/ (guards against manifest mistakes and parsing bugs).
assert_safe_destination() {
  local dest="$1"
  case "$dest" in
    "$THIRD_PARTY"/?*) ;;
    *) echo "error: refusing to write outside third_party/: '$dest'" >&2; exit 1 ;;
  esac
  case "$dest" in
    *"/.."*|*"../"*) echo "error: destination must not contain '..': '$dest'" >&2; exit 1 ;;
  esac
}

# The manifest is parsed and validated in Python; records are written as NUL-separated
# fields so that empty fields survive (bash 'read' with a whitespace IFS would drop them).
python3 - "$MANIFEST" > "$DOWNLOADS/manifest.records" <<'PY'
import json, sys
FIELDS = ["name", "url", "sha256", "kind", "archiveRoot", "destination"]
for d in json.load(open(sys.argv[1]))["dependencies"]:
    if d["kind"] not in ("zip", "tar.gz", "file"):
        sys.exit(f"unknown kind {d['kind']!r} for {d['name']}")
    if d["kind"] in ("zip", "tar.gz") and not d.get("archiveRoot"):
        sys.exit(f"{d['kind']} dependency {d['name']} needs archiveRoot")
    if "/" in d.get("archiveRoot", "") or ".." in d.get("archiveRoot", ""):
        sys.exit(f"archiveRoot of {d['name']} must be a single folder name")
    if not d.get("destination", "").startswith("third_party/"):
        sys.exit(f"destination of {d['name']} must be under third_party/")
    sys.stdout.write("".join(str(d.get(f, "")) + "\0" for f in FIELDS))
PY

while IFS= read -r -d '' name && IFS= read -r -d '' url && IFS= read -r -d '' sha \
   && IFS= read -r -d '' kind && IFS= read -r -d '' root && IFS= read -r -d '' dest; do
  dest_abs="$ROOT/$dest"
  assert_safe_destination "$dest_abs"
  stamp="$DOWNLOADS/$name.sha256"
  if [[ -f "$stamp" && "$(cat "$stamp")" == "$sha" && -e "$dest_abs" ]]; then
    echo "[deps] $name: up to date"
    continue
  fi

  file="$DOWNLOADS/$name.download"
  echo "[deps] $name: downloading $url"
  curl -sSfL --retry 3 -o "$file" "$url" </dev/null
  actual="$(sha256_of "$file")"
  if [[ "$actual" != "$sha" ]]; then
    echo "error: $name hash mismatch (expected $sha, got $actual)" >&2
    rm -f "$file"
    exit 1
  fi

  rm -rf "$dest_abs"
  mkdir -p "$(dirname "$dest_abs")"
  case "$kind" in
    zip)
      tmp="$DOWNLOADS/$name.extract"
      rm -rf "$tmp" && mkdir -p "$tmp"
      unzip -q "$file" -d "$tmp" </dev/null
      mv "$tmp/$root" "$dest_abs"
      rm -rf "$tmp"
      ;;
    tar.gz)
      tmp="$DOWNLOADS/$name.extract"
      rm -rf "$tmp" && mkdir -p "$tmp"
      tar -xzf "$file" -C "$tmp" </dev/null
      [[ -d "$tmp/$root" ]] || { echo "error: $name archive has no '$root' folder" >&2; exit 1; }
      mv "$tmp/$root" "$dest_abs"
      rm -rf "$tmp"
      ;;
    file)
      cp "$file" "$dest_abs"
      ;;
  esac
  rm -f "$file"
  echo "$sha" > "$stamp"
  echo "[deps] $name: ok"
done < "$DOWNLOADS/manifest.records"
rm -f "$DOWNLOADS/manifest.records"
