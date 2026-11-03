# The macOS tag; packages spell it 1.0~beta1 (Arch: 1.0beta1) so the beta sorts before 1.0.
VERSION := 1.0-beta1
PREFIX ?= $(HOME)/.local
# Packages install into DESTDIR with PREFIX=/usr; the paths written into files stay without it.
BINDIR := $(PREFIX)/bin
APPDIR := $(DESTDIR)$(PREFIX)/share/applications
EXTDIR := $(DESTDIR)$(PREFIX)/share/gnome-shell/extensions/youcam-capture-exclusion@youcam.app
ICONDIR := $(DESTDIR)$(PREFIX)/share/icons/hicolor
MANDIR := $(DESTDIR)$(PREFIX)/share/man/man1
PKGS := gtk+-3.0 gstreamer-1.0 gstreamer-pbutils-1.0
CFLAGS ?= -O2
CFLAGS += -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

CORE := geometry.o support.o prefs.o camera_layer.o center_stage.o picornt.o mutter_cast.o mic_level.o segment.o background.o
MEDIA := writer.o audio.o export.o
APP := main.o app.o overlay.o panel.o region.o area_overlay.o statusicon.o \
       recording.o portal.o hotkeys.o camera.o later_dialog.o $(MEDIA) $(CORE)
ICON_SIZES := 16 32 48 128 256 512

.PHONY: all test check bench-segment smoke-test install uninstall clean dist deb rpm arch

all: youcam

youcam: $(APP)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests: tests.o $(MEDIA) $(CORE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-media: test_media.o $(MEDIA) $(CORE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-segment: test_segment.o $(CORE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

bench-segment: test-segment
	./test-segment --bench

make-icon: make_icon.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(APP) tests.o test_media.o test_segment.o make_icon.o: youcam.h

segment.o: CFLAGS += -O3

# Person segmentation: MediaPipe Selfie Segmentation (vendor/selfie, Apache 2.0), converted to .ysm.
selfie_%.h: vendor/selfie/selfie_%.ysm
	cd vendor/selfie && xxd -i selfie_$*.ysm > ../../$@

background.o: selfie_landscape.h selfie_square.h

# Face detection: pico (vendor/pico, MIT) with its cascade compiled in.
facefinder.h: vendor/pico/facefinder
	cd vendor/pico && xxd -i facefinder > ../../$@

center_stage.o: facefinder.h vendor/pico/picornt.h

picornt.o: vendor/pico/picornt.c vendor/pico/picornt.h
	$(CC) $(CFLAGS) -Wno-maybe-uninitialized -c -o $@ $<

test: check test-media
	./test-media

# What package builds run: no camera, encoder or session needed.
check: tests test-segment
	./tests
	./test-segment

smoke-test: youcam
	YOUCAM_CONFIG=$$(mktemp -u)/prefs.ini ./youcam --smoke-test

icons: make-icon
	./make-icon icons $(ICON_SIZES)
	touch icons

# The desktop file gives the portal an app id, which global shortcuts require.
install: youcam icons
	install -Dm755 youcam $(DESTDIR)$(BINDIR)/youcam
	install -Dm644 youcam.1 $(MANDIR)/youcam.1
	for s in $(ICON_SIZES); do install -Dm644 icons/youcam-$$s.png $(ICONDIR)/$${s}x$${s}/apps/youcam.png; done
	sed 's|@BINDIR@|$(BINDIR)|' app.youcam.YouCam.desktop | install -Dm644 /dev/stdin $(APPDIR)/app.youcam.YouCam.desktop
	$(if $(DESTDIR),,-update-desktop-database $(APPDIR))
	rm -rf $(EXTDIR)
	install -Dm644 -t $(EXTDIR) gnome-extension/metadata.json gnome-extension/extension.js
	$(if $(DESTDIR),,-gtk-update-icon-cache -q -t $(ICONDIR))

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/youcam $(MANDIR)/youcam.1 $(APPDIR)/app.youcam.YouCam.desktop
	rm -rf $(EXTDIR)
	for s in $(ICON_SIZES); do rm -f $(ICONDIR)/$${s}x$${s}/apps/youcam.png; done

clean:
	rm -rf *.o facefinder.h selfie_*.h youcam tests test-media test-segment make-icon icons \
	       youcam-*.tar.gz *.pkg.tar.zst rpmbuild archbuild

# Packages. dist archives the committed linux/ tree (git archive from here takes only this directory).
DIST := youcam-$(VERSION)

dist:
	git archive --format=tar.gz --prefix=$(DIST)/ --add-file=../LICENSE -o $(DIST).tar.gz HEAD

deb:
	dpkg-buildpackage -b -us -uc

rpm: dist
	rpmbuild -bb packaging/youcam.spec --define "_sourcedir $(CURDIR)" --define "_topdir $(CURDIR)/rpmbuild"

arch: dist
	cd packaging && SRCDEST=$(CURDIR) PKGDEST=$(CURDIR) BUILDDIR=$(CURDIR)/archbuild makepkg -f
