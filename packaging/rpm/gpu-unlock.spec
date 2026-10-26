%global nv 610.43.03

Name: gpu-unlock
Version: 1.1.1
Release: 1%{?dist}
Summary: RTX 4090 and experimental Ada tensor unlock before NVIDIA loads
URL: https://github.com/JustAResearcher/gpu-unlock-ada
License: GPL-2.0-only
BuildArch: noarch
Source0: gpu-unlock-%{version}.tar.xz
Source1: https://download.nvidia.com/XFree86/NVIDIA-kernel-module-source/NVIDIA-kernel-module-source-%{nv}.tar.xz
Source2: https://archive.cachyos.org/archive/cachyos-v3/nvidia-utils-%{nv}-1-x86_64_v3.pkg.tar.zst
# NVIDIA inputs stay out of the source RPM (it becomes a .nosrc.rpm).
NoSource: 1
NoSource: 2
BuildRequires: make
BuildRequires: gcc
BuildRequires: python3
BuildRequires: zstd

%description
Kernel module that unlocks RTX 4090 FP32-accumulate tensor throughput before the unmodified
NVIDIA driver binds, opted in on the kernel command line.

%package dkms
Summary: RTX 4090 tensor unlock kernel source
Requires: dkms
Requires(post): dkms
Requires(preun): dkms
Requires: gpu-unlock-firmware = %{version}-%{release}

%description dkms
DKMS source for gpu_unlock, loaded ahead of nvidia on every boot. It does
nothing unless gpu_unlock.targets= is on the kernel command line.

%package firmware
Summary: Locally generated pinned NVIDIA firmware for GPU unlock
License: LicenseRef-NVIDIA AND GPL-2.0-only

%description firmware
Firmware assets generated locally from verified upstream inputs.

%prep
sha256sum -c - <<EOF
7e118923c7a23edc36114d63273a46e3e04e9af98695a42203e7ac2dfe9fc1dc  %{SOURCE1}
e17e7c92eeffa178b0fe1f40eb17a47095f8fc3ce4a77bf115cf5bd5a4df86e9  %{SOURCE2}
EOF
%autosetup
test "$(cat VERSION)" = "%{version}"

%build
%make_build INPUTS=%{_sourcedir} ASSETS="$PWD/assets" firmware

%check
make test ASSETS="$PWD/assets"

%install
make DESTDIR=%{buildroot} ASSETS="$PWD/assets" \
    install-dkms install-firmware install-dracut

%files dkms
%license licenses/GPL-2.0.txt licenses/CMP40HX-MIT.txt licenses/NVIDIA-MIT.txt
%doc %{_docdir}/gpu-unlock
%{_usrsrc}/gpu-unlock-%{version}
%{_prefix}/lib/dracut/dracut.conf.d/gpu-unlock.conf

%files firmware
%license %{_defaultlicensedir}/gpu-unlock-firmware
%{_prefix}/lib/firmware/gpu-unlock

%post dkms
# Before 1.0.1 the old package kept its DKMS entry on upgrade; drop any
# other version while its source is still on disk.
for d in /var/lib/dkms/gpu-unlock/[0-9]*; do
    [ -d "$d" ] && [ "${d##*/}" != %{version} ] &&
        dkms remove -m gpu-unlock -v "${d##*/}" --all || :
done
dkms add -m gpu-unlock -v %{version} --rpm_safe_upgrade || :

%preun dkms
# On an upgrade to the same version --rpm_safe_upgrade keeps the entry;
# on any other upgrade the old version must go too.
if [ -d /var/lib/dkms/gpu-unlock/%{version} ]; then
    dkms remove -m gpu-unlock -v %{version} --all --rpm_safe_upgrade || :
fi

%posttrans dkms
# Build for every kernel that has headers, not just the running one: a
# kernel updated but not yet booted would otherwise boot without the
# module. Kernels installed later get it from the dkms kernel-install hook.
kernels=
for b in /lib/modules/*/build; do
    [ -d "$b" ] || continue
    k=${b%/build}; k=${k##*/}
    dkms build -m gpu-unlock -v %{version} -k "$k" --force && kernels="$kernels -k $k"
done
[ -n "$kernels" ] && dkms install -m gpu-unlock -v %{version} $kernels --force || :
# The initramfs has to carry gpu_unlock and its firmware so that it runs
# before nvidia. Fedora's and EPEL's dkms regenerate every image after an
# install (post_transaction in framework.conf); otherwise rebuild those of
# the kernels that can load it.
if command -v dracut >/dev/null &&
    ! grep -qs '^post_transaction=.*dracut' /etc/dkms/framework.conf \
        /etc/dkms/framework.conf.d/*.conf; then
    for d in /lib/modules/*/; do
        k=${d%/}; k=${k##*/}
        modinfo -k "$k" gpu_unlock >/dev/null 2>&1 &&
            dracut --force --kver "$k" || :
    done
fi

%postun dkms
if [ "$1" -eq 0 ]; then
    # A kernel package's weak-modules run can still have linked another
    # kernel's build into its weak-updates; such a link now dangles and
    # makes dracut fail for that kernel.
    for l in /lib/modules/*/weak-updates/gpu_unlock.ko*; do
        [ -L "$l" ] || continue
        rm -f "$l"
        k=${l%/weak-updates/*}; depmod -a "${k##*/}" || :
    done
    # Rebuild every initramfs that still loads the removed module. dkms
    # remove may already have regenerated them, but only while the dracut
    # drop-in was still there.
    if command -v lsinitrd >/dev/null; then
        for d in /lib/modules/*/; do
            k=${d%/}; k=${k##*/}
            lsinitrd --kver "$k" 2>/dev/null | grep -q gpu_unlock &&
                dracut --force --kver "$k" || :
        done
    fi
fi

%changelog
* Fri Oct 23 2026 JustAResearcher <62031663+JustAResearcher@users.noreply.github.com> - 1.1.1-1
- Publish matched TF32 and unsigned INT4 issue-rate evidence and benchmark source.

* Fri Oct 23 2026 JustAResearcher <62031663+JustAResearcher@users.noreply.github.com> - 1.1.0-1
- Add a guarded RTX 4070 Ti SUPER Ada path and validated cold-canary report.

* Fri Oct 23 2026 Gaeros <163143799+elias-gaeros@users.noreply.github.com> - 1.0.0-1
- Add tested RTX 4090 support and credit the AD103 payload project.
