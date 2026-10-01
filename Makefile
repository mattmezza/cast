.POSIX:
CC = cc
PKG_CONFIG = pkg-config
PREFIX = /usr/local
DESTDIR =
X11 = 1
WAYLAND = 0
VERSION = 0.1.0
SOURCE_COMMIT = working-tree
RELEASE_NOTES =
LOOPBACK_DEVICE =
CFLAGS = -O2 -g
WARN = -Wall -Wextra -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes
BASE_PACKAGES = libavcodec libavformat libavutil libswscale libswresample libpipewire-0.3
PACKAGES = $(BASE_PACKAGES)
SOURCES = src/main.c src/commands.c src/config.c src/state.c src/compositor.c src/platform.c src/media.c src/webcam.c src/audio.c src/record.c vendor/inih/ini.c
INI_FLAGS = -DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 -DINI_ALLOW_MULTILINE=0 -DINI_ALLOW_INLINE_COMMENTS=0 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=8192
CPPFLAGS += -Isrc -Ivendor/inih $(INI_FLAGS) -D_GNU_SOURCE
LDLIBS += -lm -lpthread
# GNU make conditionals keep backend dependency lists completely removable.
ifeq ($(X11),1)
PACKAGES += x11 xext xrandr xi xfixes xcomposite
SOURCES += src/x11.c
CPPFLAGS += -DWITH_X11
endif
ifeq ($(WAYLAND),1)
PACKAGES += gio-2.0 gio-unix-2.0
SOURCES += src/wayland.c
CPPFLAGS += -DWITH_WAYLAND
endif
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
BUILD = build/x$(X11)-w$(WAYLAND)
OBJECTS = $(SOURCES:%.c=$(BUILD)/%.o)
CORE_SOURCES = src/config.c src/state.c vendor/inih/ini.c
MEDIA_SOURCES = src/media.c src/webcam.c src/audio.c src/record.c src/compositor.c

all: cast
FORCE:
cast: FORCE $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/%.o: %.c src/cast.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -MMD -MP -c $< -o $@
-include $(OBJECTS:.o=.d)

$(BUILD)/test_core: tests/test_core.c $(CORE_SOURCES) src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_core.c $(CORE_SOURCES) -lm
$(BUILD)/test_visual: tests/test_visual.c src/compositor.c src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(filter %.c,$^) -lm
$(BUILD)/test_media: tests/test_media.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_wayland: tests/test_wayland.c src/wayland.c src/compositor.c src/state.c src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_wayland.c src/compositor.c src/state.c $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_commands: tests/test_commands.c $(SOURCES) src/app_internal.h src/cast.h src/media_internal.h src/platform_backend.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_commands.c $(filter-out src/main.c,$(SOURCES)) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/benchmark: tests/benchmark.c src/compositor.c $(CORE_SOURCES) src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(filter %.c,$^) -lm
benchmark: $(BUILD)/benchmark
	$(BUILD)/benchmark
ifeq ($(X11),1)
XORG_TEST_SOURCES = tests/x11_smoke.c src/compositor.c src/platform.c src/x11.c
ifeq ($(WAYLAND),1)
XORG_TEST_SOURCES += src/wayland.c
endif
$(BUILD)/test_xorg: $(XORG_TEST_SOURCES) src/cast.h src/platform_backend.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(shell $(PKG_CONFIG) --cflags xtst) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(XORG_TEST_SOURCES) $(PKG_LIBS) $(shell $(PKG_CONFIG) --libs xtst) $(LDLIBS)
check-xorg: $(BUILD)/test_xorg
	timeout 30s xvfb-run -a -s '-screen 0 800x600x24' $(BUILD)/test_xorg --exercise
	timeout 30s xvfb-run -a -s '-screen 0 800x600x24 -extension MIT-SHM' $(BUILD)/test_xorg --exercise
else
check-xorg:
	@echo 'check-xorg requires X11=1 and optional Xvfb/libXtst test dependencies' >&2
	@exit 1
endif
check-unit: $(BUILD)/test_core $(BUILD)/test_visual $(BUILD)/test_media $(BUILD)/test_commands
	$(BUILD)/test_core
	$(BUILD)/test_visual
	$(BUILD)/test_media
	$(BUILD)/test_commands > $(BUILD)/commands-status.json
	python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); assert s["record"]["state"] == "stopped" and not s["record"]["finalizing"]; assert s["audio"]["mic"]["source"] == "mic \"quoted\" \\ route\n"; assert s["audio"]["desktop"]["source"] == "desktop café"; assert s["audio"]["virtual"]["name"] == "cast\tvirtual"; assert "\xff" in s["record"]["path"]' $(BUILD)/commands-status.json
check: cast check-unit
	python3 tests/test_ipc.py
check-loopback: cast
	@test -n '$(LOOPBACK_DEVICE)' || { echo 'set LOOPBACK_DEVICE to an existing v4l2loopback output device' >&2; exit 1; }
	python3 tests/test_loopback.py --device '$(LOOPBACK_DEVICE)'
ifeq ($(WAYLAND),1)
check-unit: check-wayland-unit
check-wayland-unit: $(BUILD)/test_wayland
	$(BUILD)/test_wayland --unit-only
check: check-wayland
check-wayland: $(BUILD)/test_wayland
	$(BUILD)/test_wayland
endif
sanitize:
	$(MAKE) BUILD=build/sanitize-x$(X11)-w$(WAYLAND) X11=$(X11) WAYLAND=$(WAYLAND) CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' check

install: cast
	install -Dm755 cast $(DESTDIR)$(PREFIX)/bin/cast
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/cast/LICENSE
	install -Dm644 vendor/inih/LICENSE.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/inih-LICENSE
	install -Dm644 licenses/GPL-3.0.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/GPL-3.0.txt
	install -Dm644 licenses/LGPL-2.1.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/LGPL-2.1.txt
	install -Dm644 docs/cast.1 $(DESTDIR)$(PREFIX)/share/man/man1/cast.1
	install -Dm644 examples/cast.conf $(DESTDIR)$(PREFIX)/share/doc/cast/cast.conf.example
	install -Dm644 examples/sxhkdrc $(DESTDIR)$(PREFIX)/share/doc/cast/sxhkdrc.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/cast/README.md
	@for doc in docs/*.md; do install -Dm644 "$$doc" "$(DESTDIR)$(PREFIX)/share/doc/cast/$${doc##*/}"; done
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cast $(DESTDIR)$(PREFIX)/share/man/man1/cast.1
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/cast $(DESTDIR)$(PREFIX)/share/licenses/cast
package-check:
	@test '$(VERSION)' = "$$(sed -n 's/^#define CAST_VERSION "\([^"]*\)"/\1/p' src/cast.h)" || { echo 'VERSION must match CAST_VERSION in src/cast.h' >&2; exit 1; }
package: package-check cast
	@mkdir -p dist
	rm -rf dist/stage
	$(MAKE) X11=$(X11) WAYLAND=$(WAYLAND) DESTDIR='$(CURDIR)/dist/stage' PREFIX=/usr install
	@{ echo 'cast $(VERSION)'; echo 'Source commit: $(SOURCE_COMMIT)'; echo 'Architecture:'; uname -m; echo 'Backend features: X11=$(X11) WAYLAND=$(WAYLAND)'; echo 'Runtime dynamic libraries:'; ldd cast; } > dist/stage/usr/share/doc/cast/build-info.txt
	tar -C dist/stage -czf dist/cast-$(VERSION)-linux-$$(uname -m).tar.gz .
	tar --transform='s,^,cast-$(VERSION)/,' -czf dist/cast-$(VERSION)-source.tar.gz Makefile .clang-format cast-build-prompt.md README.md LICENSE licenses src vendor tests docs examples packaging
	cd dist && sha256sum cast-$(VERSION)-linux-$$(uname -m).tar.gz cast-$(VERSION)-source.tar.gz > SHA256SUMS
release-check:
	sh packaging/release.sh check '$(VERSION)' '$(RELEASE_NOTES)' '$(X11)' '$(WAYLAND)'
release:
	sh packaging/release.sh release '$(VERSION)' '$(RELEASE_NOTES)' '$(X11)' '$(WAYLAND)'
clean:
	rm -rf build cast
.PHONY: FORCE all check check-unit check-wayland check-wayland-unit check-xorg check-loopback benchmark sanitize install uninstall package-check package release-check release clean
