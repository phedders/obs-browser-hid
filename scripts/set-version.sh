#!/bin/sh
# Set the project version everywhere it is stored.
#
#   scripts/set-version.sh 0.2.0
#   make version NEW_VERSION=0.2.0
#
# CMake and the OBS plugin manifest derive their version from VERSION at
# configure time; this script updates the remaining files and prepends a
# Debian changelog entry when needed.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

usage() {
    echo "usage: $0 X.Y.Z   (or vX.Y.Z)" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
version=${1#v}
printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || usage

printf '%s\n' "$version" > VERSION

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# Cargo package version (the first top-level version line).
awk -v ver="$version" '
    BEGIN { done = 0 }
    !done && /^version[[:space:]]*=/ {
        sub(/"[^"]*"/, "\"" ver "\"")
        done = 1
    }
    { print }
' ws-obs-client/Cargo.toml > "$tmp"
mv "$tmp" ws-obs-client/Cargo.toml

# RPM spec Version.
awk -v ver="$version" '
    BEGIN { done = 0 }
    !done && /^Version:/ {
        $0 = "Version:        " ver
        done = 1
    }
    { print }
' packaging/rpm/obs-browser-hid.spec > "$tmp"
mv "$tmp" packaging/rpm/obs-browser-hid.spec

# RPM changelog: prepend a new entry if the top entry version differs.
current_rpm_version=$(awk '
    /^%changelog/ { in_changelog = 1; next }
    in_changelog && /^\* / { print $NF; exit }
' packaging/rpm/obs-browser-hid.spec | sed 's/-[0-9][0-9]*$//')
if [ "$current_rpm_version" != "$version" ]; then
    rpm_maintainer=$(awk '
        /^%changelog/ { in_changelog = 1; next }
        in_changelog && /^\* / {
            line = $0
            sub(/^\* [A-Za-z]{3} [A-Za-z]{3} [ 0-9]{2} [0-9]{4} /, "", line)
            sub(/ - .*$/, "", line)
            print line
            exit
        }
    ' packaging/rpm/obs-browser-hid.spec)
    awk -v ver="$version" -v date="$(date '+%a %b %d %Y')" \
        -v maint="$rpm_maintainer" '
        BEGIN { done = 0 }
        /^%changelog/ {
            print
            if (!done) {
                print "* " date " " maint " - " ver "-1"
                print "- New upstream release."
                print ""
                done = 1
            }
            next
        }
        { print }
    ' packaging/rpm/obs-browser-hid.spec > "$tmp"
    mv "$tmp" packaging/rpm/obs-browser-hid.spec
fi

# Man page header: .TH ... "ws-obs X.Y.Z"
awk -v ver="$version" '
    NR == 1 { sub(/"ws-obs [^"]*"/, "\"ws-obs " ver "\"") }
    { print }
' packaging/ws-obs.1 > "$tmp"
mv "$tmp" packaging/ws-obs.1

# Debian changelog: prepend a fresh entry if the top version differs.
current_version=$(sed -n '1s/.*(\([^)]*\)).*/\1/p' debian/changelog)
if [ "$current_version" != "$version" ]; then
    maintainer=$(dpkg-parsechangelog -SMaintainer 2>/dev/null ||
        printf 'Paul Hedderly <paul+github@.mjr.org>')
    {
        printf 'obs-browser-hid (%s) unstable; urgency=medium\n\n' "$version"
        printf '  * New upstream release.\n\n'
        printf ' -- %s  %s\n\n' "$maintainer" "$(date -R)"
        cat debian/changelog
    } > "$tmp"
    mv "$tmp" debian/changelog
fi

# Keep the packaging documentation examples in step (best effort).
sed -e "s/obs-browser-hid_[0-9][0-9.]*_/obs-browser-hid_${version}_/g" \
    -e "s/ws-obs_[0-9][0-9.]*_/ws-obs_${version}_/g" \
    -e "s/obs-browser-hid-[0-9][0-9.]*-/obs-browser-hid-${version}-/g" \
    -e "s/ws-obs-[0-9][0-9.]*-/ws-obs-${version}-/g" \
    packaging/README.md > "$tmp"
mv "$tmp" packaging/README.md

# Keep the local package entry in Cargo.lock in sync so --locked builds work
# without a network round-trip for a new lockfile.
if [ -f ws-obs-client/Cargo.lock ]; then
    awk -v ver="$version" '
        /^\[\[package\]\]/ { in_pkg = 0 }
        /^name = "ws-obs"$/ { in_pkg = 1 }
        in_pkg && /^version = / {
            sub(/"[^"]*"/, "\"" ver "\"")
            in_pkg = 0
        }
        { print }
    ' ws-obs-client/Cargo.lock > "$tmp"
    mv "$tmp" ws-obs-client/Cargo.lock
fi

echo "version set to $version"
echo "next: review and commit the version changes, then: make release"
echo "      (or: make deb srpm push)"
