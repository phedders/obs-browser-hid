# obs-browser-hid / ws-obs build and release helper.
#
# Common flows:
#   make build                 plugin + CLI against installed OBS 33
#   make deb                   two .deb packages in dist/
#   make srpm                  source RPM in dist/
#   make version NEW_VERSION=0.2.0
#   make release               build, tag, push and publish a GitHub release
#
# Use OBS_USE_SYSTEM_LIBS=OFF OBS_STUDIO_DIR=/path/to/obs-studio to build
# against a local obs-studio source tree instead of installed OBS headers.

SHELL := /bin/sh

VERSION := $(strip $(shell cat VERSION 2>/dev/null))
ifeq ($(VERSION),)
$(error VERSION file is missing or empty)
endif

DIST      := dist
BUILD_DIR := build
CLI_DIR   := ws-obs-client
PREFIX    ?= /usr

OBS_USE_SYSTEM_LIBS ?= ON
OBS_STUDIO_DIR      ?= /home/prh/gits/obs-studio
RPMBUILD            ?= rpmbuild
GH                  ?= gh

ifeq ($(OBS_USE_SYSTEM_LIBS),ON)
CMAKE_OBS_FLAGS := -DOBS_USE_SYSTEM_LIBS=ON
else
CMAKE_OBS_FLAGS := -DOBS_USE_SYSTEM_LIBS=OFF \
	-DOBS_STUDIO_DIR=$(OBS_STUDIO_DIR) \
	-DOBS_BUILD_DIR=$(OBS_STUDIO_DIR)/build
endif

.PHONY: all help build plugin cli install clean dist deb source srpm \
	version tag push release remote check-version check-clean check-remote check-gh

all: build

help:
	@printf '%s\n' \
	  "obs-browser-hid $(VERSION)" \
	  "" \
	  "  make build              build the plugin and ws-obs" \
	  "  make deb                build both .deb packages into $(DIST)/" \
	  "  make source             build the Debian source package" \
	  "  make srpm               build the SRPM into $(DIST)/" \
	  "  make install            install the plugin to $(PREFIX) (may need sudo)" \
	  "  make version NEW_VERSION=x.y.z   update all version files" \
	  "  make tag                create tag v$(VERSION) at HEAD" \
	  "  make push               push HEAD and tag v$(VERSION) to origin" \
	  "  make release            build deb+srpm, push, publish GitHub release" \
	  "  make remote URL=...     set/replace the origin remote" \
	  "  make clean              remove build/package artifacts" \
	  "" \
	  "Options: OBS_USE_SYSTEM_LIBS=$(OBS_USE_SYSTEM_LIBS) OBS_STUDIO_DIR=$(OBS_STUDIO_DIR)" \
	  "         RPMBUILD=$(RPMBUILD) GH=$(GH)"

build: plugin cli

plugin:
	cmake -B $(BUILD_DIR) -S . $(CMAKE_OBS_FLAGS) -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD_DIR)

cli:
	cargo build --release --locked --manifest-path $(CLI_DIR)/Cargo.toml

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

check-version:
	scripts/check-version.sh

check-clean:
	@test -z "$$(git status --porcelain)" || { \
		echo "error: working tree is not clean; commit your changes first" >&2; \
		git status --short >&2; \
		exit 1; \
	}

check-remote:
	@git remote get-url origin >/dev/null 2>&1 || { \
		echo "error: no 'origin' remote; run: make remote URL=git@github.com:USER/REPO.git" >&2; \
		exit 1; \
	}

check-gh:
	@command -v $(GH) >/dev/null 2>&1 || { \
		echo "error: GitHub CLI ($(GH)) is not installed" >&2; \
		exit 1; \
	}
	@$(GH) auth status >/dev/null 2>&1 || { \
		echo "error: $(GH) is not authenticated; run: $(GH) auth login" >&2; \
		exit 1; \
	}

dist:
	@mkdir -p $(DIST)

deb: check-version dist
	OBS_USE_SYSTEM_LIBS=$(OBS_USE_SYSTEM_LIBS) \
	OBS_STUDIO_DIR=$(OBS_STUDIO_DIR) \
	dpkg-buildpackage -b -us -uc
	mv -f ../obs-browser-hid_$(VERSION)_*.deb $(DIST)/
	mv -f ../ws-obs_$(VERSION)_*.deb $(DIST)/
	-mv -f ../obs-browser-hid_$(VERSION)_*.changes \
		../obs-browser-hid_$(VERSION)_*.buildinfo $(DIST)/ 2>/dev/null || true
	@echo "Debian packages:"
	@ls -1 $(DIST)/*.deb

source: check-version dist
	dpkg-buildpackage -S -us -uc
	mv -f ../obs-browser-hid_$(VERSION).dsc ../obs-browser-hid_$(VERSION).tar.* $(DIST)/
	@echo "Debian source package:"
	@ls -1 $(DIST)/*.dsc

srpm: check-version dist
	RPMBUILD=$(RPMBUILD) packaging/rpm/make-srpm.sh
	cp -f packaging/rpm/rpmbuild/SRPMS/obs-browser-hid-$(VERSION)-*.src.rpm $(DIST)/
	@echo "SRPM:"
	@ls -1 $(DIST)/*.src.rpm

version:
	@test -n "$(NEW_VERSION)" || { \
		echo "usage: make version NEW_VERSION=x.y.z" >&2; \
		exit 2; \
	}
	scripts/set-version.sh "$(NEW_VERSION)"

tag: check-version check-clean
	@if git rev-parse -q --verify "refs/tags/v$(VERSION)" >/dev/null; then \
		tag_commit=$$(git rev-list -n1 "v$(VERSION)"); \
		head_commit=$$(git rev-parse HEAD); \
		if [ "$$tag_commit" != "$$head_commit" ]; then \
			echo "error: tag v$(VERSION) points to $$tag_commit, not HEAD" >&2; \
			echo "bump the version (make version NEW_VERSION=...) or delete the old tag" >&2; \
			exit 1; \
		fi; \
		echo "tag v$(VERSION) already exists at HEAD"; \
	else \
		git tag -a "v$(VERSION)" -m "obs-browser-hid $(VERSION)"; \
		echo "created tag v$(VERSION)"; \
	fi

push: check-remote check-clean check-version tag
	git push origin HEAD
	git push origin "v$(VERSION)"

# Serial order: verify, build, tag/push, then publish the release.
.NOTPARALLEL: release
release: check-clean check-version check-remote check-gh deb srpm push
	@if $(GH) release view "v$(VERSION)" >/dev/null 2>&1; then \
		$(GH) release upload "v$(VERSION)" --clobber \
			$(DIST)/*.deb $(DIST)/*.src.rpm; \
		echo "updated GitHub release v$(VERSION)"; \
	else \
		$(GH) release create "v$(VERSION)" \
			--title "obs-browser-hid $(VERSION)" \
			--generate-notes \
			$(DIST)/*.deb $(DIST)/*.src.rpm; \
		echo "created GitHub release v$(VERSION)"; \
	fi

remote:
	@test -n "$(URL)" || { \
		echo "usage: make remote URL=git@github.com:USER/REPO.git" >&2; \
		exit 2; \
	}
	@git remote add origin "$(URL)" 2>/dev/null || git remote set-url origin "$(URL)"
	@git remote -v

clean:
	rm -rf $(BUILD_DIR) $(DIST) obj-* $(CLI_DIR)/target packaging/rpm/rpmbuild
	-dh_clean
