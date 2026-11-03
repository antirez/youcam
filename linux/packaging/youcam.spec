Name:           youcam
%global tag     1.0-beta1
Version:        1.0~beta1
Release:        1%{?dist}
Summary:        Floating camera overlay and screen recorder
License:        MIT AND Apache-2.0
URL:            https://github.com/antirez/youcam
# make dist: the linux/ directory plus LICENSE.
Source0:        youcam-%{tag}.tar.gz

BuildRequires:  gcc make /usr/bin/xxd
BuildRequires:  pkgconfig(gtk+-3.0) pkgconfig(gstreamer-1.0) pkgconfig(gstreamer-pbutils-1.0)
Requires:       gstreamer1-plugins-good pipewire-gstreamer xdg-desktop-portal
# H.264 and AAC encoders: from RPM Fusion on Fedora.
Recommends:     gstreamer1-plugins-ugly gstreamer1-plugin-libav gstreamer1-plugins-bad-free wireplumber

%description
A frameless floating camera (circle, square or rectangle) with a screen
recorder for the whole screen, a display or an area. Camera transparency,
face tracking, noise reduction, automatic microphone level and background
removal, all on the CPU. Includes a GNOME Shell extension that keeps the
recording panel out of the recording.

%prep
%autosetup -n youcam-%{tag}
cp vendor/pico/LICENSE LICENSE.pico
cp vendor/selfie/LICENSE LICENSE.selfie

%build
%set_build_flags
%make_build

%check
make check

%install
%make_install PREFIX=%{_prefix}

%files
%license LICENSE LICENSE.pico LICENSE.selfie
%doc README.md CHANGELOG.md
%{_bindir}/youcam
%{_mandir}/man1/youcam.1*
%{_datadir}/applications/app.youcam.YouCam.desktop
%{_datadir}/icons/hicolor/*/apps/youcam.png
%{_datadir}/gnome-shell/extensions/youcam-capture-exclusion@youcam.app/

%changelog
* Tue Nov 03 2026 Giuseppe Patanè <giuseppe.patane@gmail.com> - 1.0~beta1-1
- First Linux release: C port of the macOS app.
