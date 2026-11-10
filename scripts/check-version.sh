#!/bin/sh
# Verify that the version-dependent files agree with the VERSION file.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

version=$(tr -d '[:space:]' < VERSION)
[ -n "$version" ] || {
    echo "error: VERSION is empty" >&2
    exit 1
}

cargo_version=$(sed -n 's/^version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    ws-obs-client/Cargo.toml | head -n 1)
debian_version=$(sed -n '1s/.*(\([^)]*\)).*/\1/p' debian/changelog)
rpm_version=$(awk '/^Version:[[:space:]]*/ { print $2; exit }' \
    packaging/rpm/obs-browser-hid.spec)
man_version=$(sed -n '1s/.*"ws-obs \([^"]*\)".*/\1/p' packaging/ws-obs.1)

status=0
check() {
    name=$1
    value=$2
    if [ "$value" != "$version" ]; then
        printf '%-18s %s (expected %s)\n' "$name:" "$value" "$version" >&2
        status=1
    fi
}

check "VERSION" "$version"
check "Cargo.toml" "$cargo_version"
check "debian/changelog" "$debian_version"
check "RPM spec" "$rpm_version"
check "man page" "$man_version"

if [ "$status" -ne 0 ]; then
    echo "version mismatch; run: make version NEW_VERSION=$version" >&2
    exit 1
fi

echo "version $version is consistent"
