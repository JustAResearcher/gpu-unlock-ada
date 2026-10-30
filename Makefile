# SPDX-License-Identifier: GPL-2.0-only
VERSION    := $(shell cat VERSION)
NV         := 610.43.03
PYTHON     ?= python3
DESTDIR    ?=
DISTDIR    ?= build
CACHE      ?= $(HOME)/.cache/gpu-unlock
INPUTS     ?= $(CACHE)/inputs
ASSETS     ?= $(CACHE)/assets
NVIDIA_SOURCE ?= $(if $(GPU_UNLOCK_NVIDIA_SOURCE),$(GPU_UNLOCK_NVIDIA_SOURCE),$(ASSETS).src/$(NV_SOURCE))
DKMSDIR    ?= /usr/src/gpu-unlock-$(VERSION)
FWDIR      ?= /usr/lib/firmware/gpu-unlock/$(NV)
LICENSEDIR ?= /usr/share/licenses
PKG        ?= gpu-unlock

MODULE_FILES   := $(filter-out %.mod.c,$(sort $(wildcard kernel/gpu_unlock/*.[ch]))) \
                  kernel/gpu_unlock/Kbuild kernel/gpu_unlock/Makefile kernel/gpu_unlock/dkms.conf
FIRMWARE_FILES := booter_load_ga102.bin gsp_ga10x.bin gsp_boot_ga102.bin \
                  gsp_boot_desc_ga102.bin v67-ga102-$(NV).bin \
                  booter_load_ad102.bin gsp_boot_ad102.bin gsp_boot_desc_ad102.bin \
                  v67-ad102-$(NV)-four-plm.bin
LICENSES       := licenses/GPL-2.0.txt licenses/CMP40HX-MIT.txt licenses/NVIDIA-MIT.txt
NV_SOURCE      := NVIDIA-kernel-module-source-$(NV)
NV_UTILS       := nvidia-utils-$(NV)-1-x86_64_v3.pkg.tar.zst

TARBALL := $(abspath $(DISTDIR))/gpu-unlock-$(VERSION).tar.xz
DEBDIR  := $(abspath $(DISTDIR))/deb
RPMDIR  := $(abspath $(DISTDIR))/rpm
DEBS    := $(foreach p,dkms firmware,$(DEBDIR)/gpu-unlock-$(p)_$(VERSION)_all.deb)
VOIDDIR := $(abspath $(DISTDIR))/void
VOIDSRC := $(VOIDDIR)/gpu-unlock-$(VERSION)
XBPSREV ?= 1

.PHONY: all test test-full dist inputs firmware assets install-dkms install-firmware \
        install-mkinitcpio install-initramfs-tools install-dracut \
        install-licenses arch deb rpm void install install-arch install-deb install-rpm \
        install-void

all:

# With generated or installed firmware, the tests also run the whole unlock
# against mocked hardware.
test:
	GPU_UNLOCK_ASSETS="$(ASSETS)" GPU_UNLOCK_NVIDIA_SOURCE="$(NVIDIA_SOURCE)" $(PYTHON) -m unittest discover -s tests -v

# Refuse a green pre-hardware check that omits the real-asset transactions.
test-full:
	GPU_UNLOCK_REQUIRE_INPUTS=1 $(MAKE) test

# Source-only release tarball; see .gitattributes for what is left out.
dist:
	mkdir -p $(DISTDIR)
	git archive --format=tar --prefix=gpu-unlock-$(VERSION)/ \
		--output=$(DISTDIR)/gpu-unlock-$(VERSION).tar HEAD
	xz -9 -f $(DISTDIR)/gpu-unlock-$(VERSION).tar

# Download and SHA-256 check the pinned NVIDIA inputs. makepkg does this
# itself from the PKGBUILD's source array.
inputs:
	$(PYTHON) firmware/fetch_artifacts.py $(INPUTS)

# Generate firmware from $(INPUTS) into $(ASSETS); debian/rules and the RPM
# spec call this from their build step.
firmware:
	rm -rf $(ASSETS).src
	mkdir -p $(ASSETS).src
	tar -xf $(INPUTS)/$(NV_SOURCE).tar.xz -C $(ASSETS).src
	$(PYTHON) firmware/generate_v67.py "$$(realpath $(INPUTS)/$(NV_UTILS))" $(ASSETS)
	$(PYTHON) firmware/extract_boot_assets.py $(ASSETS).src/$(NV_SOURCE) $(ASSETS)
	$(PYTHON) firmware/extract_boot_assets.py $(ASSETS).src/$(NV_SOURCE) $(ASSETS) --chip ad102

assets: inputs
	$(MAKE) firmware

# The module source. It does nothing unless gpu_unlock.targets= is on the
# kernel command line.
install-dkms:
	grep -qx 'PACKAGE_VERSION="$(VERSION)"' kernel/gpu_unlock/dkms.conf
	grep -q 'MODULE_VERSION("$(VERSION)")' kernel/gpu_unlock/main.c
	install -Dm644 -t $(DESTDIR)$(DKMSDIR) $(MODULE_FILES)
	install -Dm644 README.md $(DESTDIR)/usr/share/doc/gpu-unlock/README.md

# The generators check every file against its pinned SHA-256 before writing it.
install-firmware:
	install -Dm644 -t $(DESTDIR)$(FWDIR) $(addprefix $(ASSETS)/,$(FIRMWARE_FILES))
	install -Dm644 $(ASSETS)/NVIDIA-LICENSE $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/NVIDIA-LICENSE
	install -Dm644 firmware/NOTICE.md $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/NOTICE.md
	install -Dm644 licenses/GPL-2.0.txt $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/GPL-2.0.txt

# One file per initramfs generator: include gpu_unlock and its firmware in
# the image so it runs before an nvidia module loaded from the initramfs.
install-mkinitcpio:
	install -Dm644 packaging/boot/mkinitcpio.conf $(DESTDIR)/etc/mkinitcpio.conf.d/zz-gpu-unlock.conf

install-initramfs-tools:
	install -Dm644 packaging/boot/initramfs-tools.conf \
		$(DESTDIR)/usr/share/initramfs-tools/modules.d/gpu-unlock.conf

install-dracut:
	install -Dm644 packaging/boot/dracut.conf $(DESTDIR)/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf

install-licenses:
	install -Dm644 -t $(DESTDIR)$(LICENSEDIR)/$(PKG) $(LICENSES)

# Native packages, built from the committed HEAD like the PKGBUILD. The
# install-* variants also pull build dependencies and install the result.
install:
	@set -eu; \
	if [ -r /etc/os-release ]; then \
		. /etc/os-release; \
	elif [ -r /usr/lib/os-release ]; then \
		. /usr/lib/os-release; \
	fi; \
	for distro in $${ID:-} $${ID_LIKE:-}; do \
		case "$$distro" in \
			arch) target=arch ;; \
			debian|ubuntu) target=deb ;; \
			fedora|rhel|centos) target=rpm ;; \
			void) target=void ;; \
			*) continue ;; \
		esac; \
		echo "Detected $$distro family; running make install-$$target"; \
		$(MAKE) install-$$target; \
		exit $$?; \
	done; \
	echo "Unsupported distribution: $${ID:-unknown}. Use make install-arch, install-deb, install-rpm, or install-void explicitly (RPM requires DNF)." >&2; \
	exit 1

arch:
	cd packaging/arch && makepkg -f

install-arch:
	cd packaging/arch && makepkg -sif

deb: dist inputs
	rm -rf $(DEBDIR)
	mkdir -p $(DEBDIR)
	tar -xf $(TARBALL) -C $(DEBDIR)
	cd $(DEBDIR)/gpu-unlock-$(VERSION) && \
		GPU_UNLOCK_INPUTS=$(abspath $(INPUTS)) dpkg-buildpackage -us -uc

install-deb:
	sudo apt-get build-dep -y $(CURDIR)
	$(MAKE) deb
	sudo apt-get install -y $(DEBS)

rpm: dist inputs
	rm -rf $(RPMDIR)
	mkdir -p $(RPMDIR)/SOURCES
	cp $(TARBALL) $(RPMDIR)/SOURCES/
	ln -s $(abspath $(INPUTS))/$(NV_SOURCE).tar.xz $(abspath $(INPUTS))/$(NV_UTILS) $(RPMDIR)/SOURCES/
	rpmbuild -ba --define "_topdir $(RPMDIR)" packaging/rpm/gpu-unlock.spec

install-rpm:
	sudo dnf install -y rpm-build
	sudo dnf builddep -y packaging/rpm/gpu-unlock.spec
	$(MAKE) rpm
	sudo dnf install -y $(RPMDIR)/RPMS/noarch/gpu-unlock-*-$(VERSION)-*.noarch.rpm

# xbps packages in a local repository under $(VOIDDIR)/repo. The scripts
# in packaging/void register the module with DKMS through the dkms trigger
# and rebuild the initramfs, like a void-packages template would.
void: dist inputs
	rm -rf $(VOIDDIR)
	mkdir -p $(VOIDDIR)/repo
	tar -xf $(TARBALL) -C $(VOIDDIR)
	$(MAKE) -C $(VOIDSRC) INPUTS=$(abspath $(INPUTS)) ASSETS=$(VOIDSRC)/assets firmware
	$(MAKE) -C $(VOIDSRC) ASSETS=$(VOIDSRC)/assets test-full
	$(MAKE) -C $(VOIDSRC) DESTDIR=$(VOIDDIR)/dkms \
		install-dkms install-dracut install-mkinitcpio
	$(MAKE) -C $(VOIDSRC) DESTDIR=$(VOIDDIR)/dkms PKG=gpu-unlock-dkms install-licenses
	$(MAKE) -C $(VOIDSRC) DESTDIR=$(VOIDDIR)/firmware ASSETS=$(VOIDSRC)/assets install-firmware
	for f in INSTALL REMOVE; do \
		sed 's/@VERSION@/$(VERSION)/' $(VOIDSRC)/packaging/void/$$f > $(VOIDDIR)/dkms/$$f || exit 1; \
	done
	install -m644 $(VOIDSRC)/packaging/void/INSTALL.msg $(VOIDDIR)/dkms/INSTALL.msg
	cd $(VOIDDIR)/repo && xbps-create -A noarch -n gpu-unlock-firmware-$(VERSION)_$(XBPSREV) \
		-s 'Locally generated, pinned NVIDIA $(NV) firmware assets for gpu-unlock' \
		-l 'custom:NVIDIA, GPL-2.0-only' -H https://github.com/JustAResearcher/gpu-unlock-ada \
		$(VOIDDIR)/firmware
	cd $(VOIDDIR)/repo && xbps-create -A noarch -n gpu-unlock-dkms-$(VERSION)_$(XBPSREV) \
		-s 'RTX 4090 and experimental Ada tensor unlock before NVIDIA loads' \
		-l GPL-2.0-only -H https://github.com/JustAResearcher/gpu-unlock-ada \
		-D 'dkms>=0 gpu-unlock-firmware-$(VERSION)_$(XBPSREV)' \
		-F /etc/mkinitcpio.conf.d/zz-gpu-unlock.conf \
		$(VOIDDIR)/dkms
	xbps-rindex -a $(VOIDDIR)/repo/*.xbps

install-void:
	sudo xbps-install -Sy git python3 zstd xz make gcc
	$(MAKE) void
	sudo xbps-install -fy --repository=$(VOIDDIR)/repo gpu-unlock-dkms gpu-unlock-firmware
