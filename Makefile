.POSIX:
CC = cc
PKG_CONFIG = pkg-config
PREFIX = /usr/local
DESTDIR =
X11 = 1
WAYLAND = 0
VERSION = 0.1.0
CFLAGS = -O2 -g
WARN = -Wall -Wextra -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes
BASE_PACKAGES = libavcodec libavformat libavutil libswscale libswresample libpipewire-0.3
PACKAGES = $(BASE_PACKAGES)
SOURCES = src/main.c src/config.c src/state.c src/compositor.c src/platform.c src/media.c src/webcam.c src/audio.c src/record.c vendor/inih/ini.c
INI_FLAGS = -DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 -DINI_ALLOW_MULTILINE=0 -DINI_ALLOW_INLINE_COMMENTS=0 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=8192
CPPFLAGS += -Isrc -Ivendor/inih $(INI_FLAGS) -D_GNU_SOURCE
LDLIBS += -lm -lpthread
# GNU make conditionals keep backend dependency lists completely removable.
ifeq ($(X11),1)
PACKAGES += x11 xext xrandr xi xfixes
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
$(BUILD)/test_visual: tests/test_visual.c src/compositor.c src/state.c src/config.c vendor/inih/ini.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $^ -lm
$(BUILD)/test_media: tests/test_media.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $^ $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_wayland: tests/test_wayland.c src/wayland.c src/compositor.c src/state.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_wayland.c src/compositor.c src/state.c $(PKG_LIBS) $(LDLIBS)
check: cast $(BUILD)/test_core $(BUILD)/test_visual $(BUILD)/test_media
	$(BUILD)/test_core
	$(BUILD)/test_visual
	$(BUILD)/test_media
	python3 tests/test_ipc.py
ifeq ($(WAYLAND),1)
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
package: cast
	@mkdir -p dist
	rm -rf dist/stage
	$(MAKE) X11=$(X11) WAYLAND=$(WAYLAND) DESTDIR='$(CURDIR)/dist/stage' PREFIX=/usr install
	@{ echo 'cast $(VERSION)'; echo 'Architecture:'; uname -m; echo 'Backend features: X11=$(X11) WAYLAND=$(WAYLAND)'; echo 'Runtime dynamic libraries:'; ldd cast; } > dist/stage/usr/share/doc/cast/build-info.txt
	tar -C dist/stage -czf dist/cast-$(VERSION)-linux-$$(uname -m).tar.gz .
	tar --transform='s,^,cast-$(VERSION)/,' -czf dist/cast-$(VERSION)-source.tar.gz Makefile README.md LICENSE licenses src vendor tests docs examples packaging
	cd dist && sha256sum cast-$(VERSION)-linux-*.tar.gz cast-$(VERSION)-source.tar.gz > SHA256SUMS
clean:
	rm -rf build cast
.PHONY: FORCE all check check-wayland sanitize install uninstall package clean
