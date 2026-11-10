# Packaging

Two binary packages are produced from this source tree:

| Package | Contents |
|---------|----------|
| `obs-browser-hid` | OBS plugin module + plugin manifest |
| `ws-obs` | command-line obs-websocket client |

The plugin follows OBS 33's third-party plugin layout:

```
<libdir>/obs-modules/plugins/obs-browser-hid.so
<datadir>/obs/obs-modules/plugins/obs-browser-hid/manifest.json
```

On Debian `<libdir>` is `lib/<multiarch>` (e.g. `lib/x86_64-linux-gnu`); on
RPM distributions it is normally `lib64` on 64-bit systems.

The old `obs-modules/core` directory is for OBS itself; third-party modules
must not go there under OBS 33+.

If you previously installed this plugin manually into
`<libdir>/obs-modules/core/` (the pre-OBS-33 third-party location), remove
that copy before installing the packages / restarting OBS, otherwise OBS may
see two copies of the module:

```sh
sudo rm -f /usr/lib/<multiarch>/obs-modules/core/obs-browser-hid.so
```

## Makefile quick reference

```sh
make build          # plugin + CLI
make deb            # both .deb packages in dist/
make source         # Debian source package in dist/
make srpm           # SRPM in dist/
make version NEW_VERSION=0.2.0
make tag
make push
make release
```

`make build`/`make deb` use installed OBS 33 headers by default. For a source
tree:

```sh
make build OBS_USE_SYSTEM_LIBS=OFF OBS_STUDIO_DIR=/path/to/obs-studio
```

`make release` requires a clean tree, an `origin` remote and an authenticated
GitHub CLI (`gh auth login`). It builds the Debian and RPM artifacts, creates
and pushes tag `vX.Y.Z`, then creates/updates the GitHub release with the
`.deb` and `.src.rpm` files. Use `make remote URL=...` once to set `origin`.

## Version numbers

`VERSION` is the single source of truth. CMake reads it (and generates the
plugin manifest version from it), and:

```sh
make version NEW_VERSION=0.2.0
```

updates `VERSION`, `ws-obs-client/Cargo.toml`, `debian/changelog` (adds a new
entry), `packaging/rpm/obs-browser-hid.spec` and the `ws-obs` man page.
Review and commit the result before releasing. `make check-version` verifies
that the files agree; release tags are `vX.Y.Z`.

## Debian / Ubuntu

Requirements:

- OBS 33 installed with its development headers/libraries (the self-built
  `obs-studio` package in this environment provides `/usr/include/obs` and
  the `libobs`/`obs-frontend-api` libraries), **or** an already-built
  obs-studio source tree;
- `debhelper`, `cmake`, `pkg-config`, `libxkbcommon-dev`, `qt6-base-dev`,
  `cargo`, `rustc`.

Build both binary packages from the repository root:

```sh
dpkg-buildpackage -b -us -uc
```

The `.deb` files are written to the parent directory, for example:

```
../obs-browser-hid_0.1.0_amd64.deb
../ws-obs_0.1.0_amd64.deb
```

By default the build uses the system OBS headers/libraries
(`OBS_USE_SYSTEM_LIBS=ON` in `debian/rules`). To build against a source tree
instead:

```sh
OBS_USE_SYSTEM_LIBS=OFF OBS_STUDIO_DIR=/home/prh/gits/obs-studio \
    dpkg-buildpackage -b -us -uc
```

Install with `sudo apt install ../obs-browser-hid_*.deb ../ws-obs_*.deb`
(or `dpkg -i`). OBS must be restarted after installing the plugin.

## RPM (Fedora, openSUSE, ...)

Requirements:

- `rpm-build`/`rpmdevtools`, `cmake`, `gcc`, `gcc-c++`, `make`,
  `pkgconfig(xkbcommon)`, `pkgconfig(Qt6Widgets)`, `cargo`, `rust`;
- OBS 33 development files (`pkgconfig(libobs)`, `pkgconfig(obs-frontend-api)`)
  **or** an already-built obs-studio source tree.

Create the SRPM:

```sh
packaging/rpm/make-srpm.sh
```

This writes the spec and tarball under `packaging/rpm/rpmbuild/` and produces:

```
packaging/rpm/rpmbuild/SRPMS/obs-browser-hid-0.1.0-*.src.rpm
```

Build the binary RPMs from it:

```sh
rpmbuild --define "_topdir $(pwd)/packaging/rpm/rpmbuild" \
    --rebuild packaging/rpm/rpmbuild/SRPMS/obs-browser-hid-0.1.0-*.src.rpm
```

If OBS is not installed as a development package, use a built obs-studio tree:

```sh
rpmbuild --define "_topdir $(pwd)/packaging/rpm/rpmbuild" \
    --rebuild packaging/rpm/rpmbuild/SRPMS/obs-browser-hid-0.1.0-*.src.rpm \
    --define 'obs_studio_dir /path/to/obs-studio'
```

The resulting RPMs are written under
`packaging/rpm/rpmbuild/RPMS/<arch>/`:

```
obs-browser-hid-0.1.0-*.rpm
ws-obs-0.1.0-*.rpm
```

Notes:

- Build the binary RPMs on an RPM-based host (Fedora/openSUSE, or a
  compatible container): the spec uses the distro `%cmake`, `%cmake_build`
  and `%cmake_install` macros. Creating the SRPM works anywhere `rpmbuild`
  is available.
- The Rust client is built with `cargo build --release --locked`. The SRPM
  includes `Cargo.lock`; building still needs crates.io access (or a warm
  cargo cache), because the crates are not vendored.
- RPM's automatic dependency generator will require the OBS libraries
  (`libobs.so.*`, `libobs-frontend-api.so.*`). OBS must be installed in a way
  that satisfies those dependencies (normally the distro `obs-studio`
  package). For a self-built OBS outside RPM, install with `--nodeps` only if
  you understand the risk.
- `ws-obs` is independent of the plugin and only recommends it; it can be
  installed on a control machine that talks to OBS over the network.

## Manual (no package manager)

```sh
cmake -B build -S . -DOBS_USE_SYSTEM_LIBS=ON
cmake --build build
sudo cmake --install build --prefix /usr
```

This installs the same two paths as the packages. For development runs
against a local obs-studio checkout, omit `-DOBS_USE_SYSTEM_LIBS=ON` (the tree
paths default to `/home/prh/gits/obs-studio`; override with
`-DOBS_STUDIO_DIR=...`).
