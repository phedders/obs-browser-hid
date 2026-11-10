#!/bin/sh
# Build a source RPM for obs-browser-hid (which yields the plugin and ws-obs
# binary RPMs). Run from anywhere inside the git checkout.
#
#   packaging/rpm/make-srpm.sh
#   rpmbuild --rebuild packaging/rpm/rpmbuild/SRPMS/obs-browser-hid-1.0.0-*.src.rpm
#
# If OBS 33 development files are not installed system-wide, rebuild against
# an existing obs-studio build tree:
#
#   rpmbuild --rebuild ... \
#       --define 'obs_studio_dir /home/prh/gits/obs-studio'
#
# The Rust client build currently expects access to crates.io (or a populated
# cargo registry cache); the Cargo.lock is included in the source tarball.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
name=obs-browser-hid
spec="$root/packaging/rpm/$name.spec"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild is required (install rpm-build or rpmdevtools)" >&2
    exit 1
fi

if ! command -v git >/dev/null 2>&1 || ! git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    echo "this script must be run from a git checkout (it uses git archive)" >&2
    exit 1
fi

version=$(awk '/^Version:[[:space:]]*/ {print $2; exit}' "$spec")
if [ -z "$version" ]; then
    echo "could not determine Version from $spec" >&2
    exit 1
fi

if ! git -C "$root" diff --quiet --ignore-submodules HEAD --; then
    echo "warning: uncommitted changes are not included (building from HEAD)" >&2
fi

topdir=${RPM_TOPDIR:-"$root/packaging/rpm/rpmbuild"}
mkdir -p "$topdir/BUILD" "$topdir/BUILDROOT" "$topdir/RPMS" \
         "$topdir/SOURCES" "$topdir/SPECS" "$topdir/SRPMS"

git -C "$root" archive --format=tar.gz --prefix="$name-$version/" \
    -o "$topdir/SOURCES/$name-$version.tar.gz" HEAD
cp "$spec" "$topdir/SPECS/"

rpmbuild --define "_topdir $topdir" -bs "$topdir/SPECS/$name.spec"

echo
echo "SRPM(s):"
find "$topdir/SRPMS" -maxdepth 1 -name '*.src.rpm' -print
