Name:           obs-browser-hid
Version:        1.0.0
Release:        1%{?dist}
Summary:        Browser source input bridge for OBS Studio
License:        GPL-2.0-or-later
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  cargo
BuildRequires:  rust
# OBS 33 development files are required. Either install the distro's
# obs-studio development package (providing these pkg-config modules), or
# build against an already-built obs-studio tree by defining the macro:
#   rpmbuild --define 'obs_studio_dir /path/to/obs-studio' ...
%{!?obs_studio_dir:BuildRequires: pkgconfig(libobs)}
%{!?obs_studio_dir:BuildRequires: pkgconfig(obs-frontend-api)}

%description
Adds obs-websocket vendor requests and an OBS dock that send synthetic
keyboard, text and mouse input to named OBS browser sources. Quick actions
are configurable in the dock, can be bound to OBS frontend hotkeys,
imported/exported, and can capture click positions from the source's
interact window.

The module is installed in the OBS 33+ third-party plugin location
(%{_libdir}/obs-modules/plugins) with its manifest under
%{_datadir}/obs/obs-modules/plugins/obs-browser-hid.

%package -n ws-obs
Summary:        Command-line client for OBS obs-websocket vendor requests
Recommends:     %{name} = %{version}-%{release}

%description -n ws-obs
A small WebSocket client for OBS obs-websocket v5. It supports the
obs-browser-hid vendor (keys, text, mouse clicks and the virtual sources
"current" and "interact") as well as generic CallVendorRequest calls.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake \
    -DOBS_PLUGIN_INSTALL_DIR=%{_libdir}/obs-modules/plugins \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    %{?obs_studio_dir:-DOBS_STUDIO_DIR=%{obs_studio_dir} -DOBS_BUILD_DIR=%{obs_studio_dir}/build} \
    %{!?obs_studio_dir:-DOBS_USE_SYSTEM_LIBS=ON}
%cmake_build
pushd ws-obs-client
cargo build --release --locked
popd

%install
%cmake_install
install -D -m 0755 ws-obs-client/target/release/ws-obs %{buildroot}%{_bindir}/ws-obs
install -D -m 0644 packaging/ws-obs.1 %{buildroot}%{_mandir}/man1/ws-obs.1

%files
%license LICENSE
%doc README.md DEVELOPMENT.md
%{_libdir}/obs-modules/plugins/obs-browser-hid.so
%{_datadir}/obs/obs-modules/plugins/obs-browser-hid/manifest.json

%files -n ws-obs
%license LICENSE
%doc README.md
%{_bindir}/ws-obs
%{_mandir}/man1/ws-obs.1*

%changelog
* Tue Nov 10 2026 Paul Hedderly <paul+github@.mjr.org> - 1.0.0-1
- Initial RPM packaging.
