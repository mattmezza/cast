.POSIX:
CC = cc
PKG_CONFIG = pkg-config
PREFIX = /usr/local
DESTDIR =
X11 = 1
WAYLAND = 0
PANEL = 0
VERSION = 0.7.0
SOURCE_COMMIT = working-tree
RELEASE_NOTES =
RELEASE_TAG = v$(VERSION)
LOOPBACK_DEVICE =
CFLAGS = -O2 -g
WARN = -Wall -Wextra -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes
BASE_PACKAGES = fontconfig freetype2 libavcodec libavformat libavutil libswscale libswresample libpipewire-0.3
PACKAGES = $(BASE_PACKAGES)
SOURCES = src/main.c src/commands.c src/config.c src/state.c src/compositor.c src/composition_assets.c src/presentation_text.c src/platform.c src/media.c src/webcam.c src/audio.c src/record.c src/stream.c src/panel_transport.c src/help_commands.c src/update.c vendor/inih/ini.c
INI_FLAGS = -DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 -DINI_ALLOW_MULTILINE=0 -DINI_ALLOW_INLINE_COMMENTS=0 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=8192
CPPFLAGS += -Isrc -Ivendor/inih $(INI_FLAGS) -D_GNU_SOURCE
LDLIBS += -lm -lpthread
# GNU make conditionals keep backend dependency lists completely removable.
ifeq ($(X11),1)
PACKAGES += x11 xext xrandr xi xfixes xcomposite
SOURCES += src/x11.c src/preview_text.c
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
COMMAND_ASSETS = $(BUILD)/src/command_assets.o
FONT_OBJECT =
ifneq ($(filter 1,$(X11) $(PANEL)),)
FONT_OBJECT = $(BUILD)/src/panel_font.o
endif
OBJECTS = $(SOURCES:%.c=$(BUILD)/%.o) $(COMMAND_ASSETS) $(FONT_OBJECT)
ifeq ($(PANEL),1)
# Overridable for a locally built SDK; normal builds use system SDL3/SDL3_ttf.
ifeq ($(origin PANEL_CFLAGS):$(origin PANEL_LIBS),undefined:undefined)
ifeq ($(shell $(PKG_CONFIG) --exists sdl3 sdl3-ttf && echo yes),)
$(error PANEL=1 requires SDL3 and SDL3_ttf. On Arch run: sudo pacman -S --needed sdl3 sdl3_ttf. Then retry make PANEL=1)
endif
endif
PANEL_CFLAGS = $(shell $(PKG_CONFIG) --cflags sdl3 sdl3-ttf)
PANEL_LIBS = $(shell $(PKG_CONFIG) --libs sdl3 sdl3-ttf)
SOURCES += src/panel.c
CPPFLAGS += -DWITH_PANEL -Ivendor/clay $(PANEL_CFLAGS)
LDLIBS += $(PANEL_LIBS)
BUILD = build/x$(X11)-w$(WAYLAND)-p1

endif
CORE_SOURCES = src/compositor.c src/composition_assets.c src/presentation_text.c src/config.c src/state.c vendor/inih/ini.c
MEDIA_SOURCES = src/media.c src/webcam.c src/audio.c src/record.c src/stream.c src/compositor.c src/composition_assets.c src/presentation_text.c

all: cast
FORCE:
cast: FORCE $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/%.o: %.c src/cast.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -MMD -MP -c $< -o $@
$(BUILD)/src/panel_font.o: src/panel_font.S assets/fonts/Inter.ttf
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@
$(BUILD)/src/command_assets.o: src/command_assets.S completions/cast.bash completions/_cast completions/cast.fish packaging/install.sh
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@
-include $(OBJECTS:.o=.d)

$(BUILD)/test_core: tests/test_core.c $(CORE_SOURCES) src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_core.c $(CORE_SOURCES) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_visual: tests/test_visual.c src/compositor.c src/composition_assets.c src/presentation_text.c src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_media: tests/test_media.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_stream: tests/test_stream.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h src/stream.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS)
check-stream: $(BUILD)/test_stream
	$(BUILD)/test_stream
	python3 tests/test_stream_network.py --binary $(BUILD)/test_stream

$(BUILD)/test_wayland: tests/test_wayland.c src/wayland.c src/compositor.c src/composition_assets.c src/presentation_text.c src/state.c src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_wayland.c src/compositor.c src/composition_assets.c src/presentation_text.c src/state.c $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_commands: tests/test_commands.c $(SOURCES) src/app_internal.h src/cast.h src/media_internal.h src/platform_backend.h $(COMMAND_ASSETS) $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_commands.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(COMMAND_ASSETS) $(FONT_OBJECT) $(PKG_LIBS) $(LDLIBS)
$(BUILD)/test_panel_transport: tests/test_panel_transport.c $(SOURCES) src/app_internal.h src/panel_transport.h $(COMMAND_ASSETS) $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_panel_transport.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(COMMAND_ASSETS) $(FONT_OBJECT) $(PKG_LIBS) $(LDLIBS)
ifeq ($(PANEL),1)
$(BUILD)/test_panel_routes: tests/test_panel_routes.c $(SOURCES) src/panel_transport.h $(BUILD)/src/panel_font.o $(COMMAND_ASSETS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_panel_routes.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(BUILD)/src/panel_font.o $(COMMAND_ASSETS) $(PKG_LIBS) $(LDLIBS)
check-unit: check-panel-routes
check-panel-routes: $(BUILD)/test_panel_routes
	$(BUILD)/test_panel_routes
endif
$(BUILD)/benchmark: tests/benchmark.c $(CORE_SOURCES) src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS)
benchmark: $(BUILD)/benchmark
	$(BUILD)/benchmark
ifeq ($(X11),1)
XORG_TEST_SOURCES = tests/x11_smoke.c src/compositor.c src/composition_assets.c src/presentation_text.c src/platform.c src/x11.c src/preview_text.c
ifeq ($(WAYLAND),1)
XORG_TEST_SOURCES += src/wayland.c
endif
$(BUILD)/test_xorg: $(XORG_TEST_SOURCES) src/cast.h src/platform_backend.h $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(shell $(PKG_CONFIG) --cflags xtst) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(XORG_TEST_SOURCES) $(FONT_OBJECT) $(PKG_LIBS) $(shell $(PKG_CONFIG) --libs xtst) $(LDLIBS)
check-xorg: cast $(BUILD)/test_xorg
	timeout 30s xvfb-run -a -s '-screen 0 800x600x24' $(BUILD)/test_xorg --exercise
	timeout 30s xvfb-run -a -s '-screen 0 800x600x24 -extension MIT-SHM' $(BUILD)/test_xorg --exercise
	CAST_COUNTDOWN_WITH_PANEL=$(PANEL) timeout 45s xvfb-run -a -s '-screen 0 800x600x24' python3 tests/test_countdown_xorg.py
else
check-xorg:
	@echo 'check-xorg requires X11=1 and optional Xvfb/libXtst test dependencies' >&2
	@exit 1
endif
check-unit: check-stream $(BUILD)/test_core $(BUILD)/test_visual $(BUILD)/test_media $(BUILD)/test_commands $(BUILD)/test_panel_transport
	$(BUILD)/test_core
	$(BUILD)/test_visual
	$(BUILD)/test_media
	$(BUILD)/test_commands > $(BUILD)/commands-status.json
	$(BUILD)/test_panel_transport
	python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); assert s["record"]["state"] == "stopped" and not s["record"]["finalizing"]; assert s["audio"]["mic"]["source"] == "mic \"quoted\" \\ route\n"; assert s["audio"]["desktop"]["source"] == "desktop café"; assert s["audio"]["virtual"]["name"] == "cast\tvirtual"; assert "\xff" in s["record"]["path"]' $(BUILD)/commands-status.json
check: cast check-unit
	python3 tests/test_ipc.py
	python3 tests/test_output_modes.py
	python3 tests/test_three_outputs.py
	python3 tests/test_presentation_layers.py
	python3 tests/test_help_commands.py
	python3 tests/test_install.py
check-loopback: cast
	@test -n '$(LOOPBACK_DEVICE)' || { echo 'set LOOPBACK_DEVICE to an existing v4l2loopback output device' >&2; exit 1; }
	python3 tests/test_loopback.py --device '$(LOOPBACK_DEVICE)'
ifeq ($(PANEL),1)
check-panel: cast
	python3 tests/test_panel_ui.py
else
check-panel:
	@echo 'check-panel requires PANEL=1 and optional Xvfb/xdotool/xclip test dependencies' >&2
	@exit 1
endif
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
	install -Dm644 licenses/FreeType-FTL.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/FreeType-FTL
	install -Dm644 licenses/Fontconfig-COPYING.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/Fontconfig-COPYING
	install -Dm644 docs/cast.1 $(DESTDIR)$(PREFIX)/share/man/man1/cast.1
	install -Dm644 examples/cast.conf $(DESTDIR)$(PREFIX)/share/doc/cast/cast.conf.example
	install -Dm644 examples/sxhkdrc $(DESTDIR)$(PREFIX)/share/doc/cast/sxhkdrc.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/cast/README.md
	install -Dm644 completions/cast.bash $(DESTDIR)$(PREFIX)/share/bash-completion/completions/cast
	install -Dm644 completions/_cast $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_cast
	install -Dm644 completions/cast.fish $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/cast.fish
ifeq ($(PANEL),1)
	install -Dm644 vendor/clay/LICENSE.md $(DESTDIR)$(PREFIX)/share/licenses/cast/clay-LICENSE
	install -Dm644 licenses/SDL3_ttf-ZLIB.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/SDL3_ttf-ZLIB
endif
ifneq ($(filter 1,$(X11) $(PANEL)),)
	install -Dm644 assets/fonts/OFL.txt $(DESTDIR)$(PREFIX)/share/licenses/cast/Inter-OFL
endif
	@for doc in docs/*.md; do install -Dm644 "$$doc" "$(DESTDIR)$(PREFIX)/share/doc/cast/$${doc##*/}"; done
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cast $(DESTDIR)$(PREFIX)/share/man/man1/cast.1
	rm -f $(DESTDIR)$(PREFIX)/share/bash-completion/completions/cast $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_cast $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/cast.fish
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/cast $(DESTDIR)$(PREFIX)/share/licenses/cast
package-check:
	@test '$(VERSION)' = "$$(sed -n 's/^#define CAST_VERSION "\([^"]*\)"/\1/p' src/cast.h)" || { echo 'VERSION must match CAST_VERSION in src/cast.h' >&2; exit 1; }
package: package-check cast
	@mkdir -p dist
	rm -rf dist/stage
	$(MAKE) X11=$(X11) WAYLAND=$(WAYLAND) PANEL=$(PANEL) DESTDIR='$(CURDIR)/dist/stage' PREFIX=/usr install
	@{ echo 'cast $(VERSION)'; echo 'Source commit: $(SOURCE_COMMIT)'; echo 'Architecture:'; uname -m; echo 'Backend features: X11=$(X11) WAYLAND=$(WAYLAND) PANEL=$(PANEL)'; echo 'Runtime dynamic libraries:'; ldd cast; } > dist/stage/usr/share/doc/cast/build-info.txt
	tar -C dist/stage -czf dist/cast-$(VERSION)-linux-$$(uname -m).tar.gz .
	tar --transform='s,^,cast-$(VERSION)/,' -czf dist/cast-$(VERSION)-source.tar.gz Makefile .clang-format cast-build-prompt.md redesign-prompt.md streaming-prompt.md README.md PRODUCT.md DESIGN.md LICENSE licenses src vendor assets completions tests docs examples packaging .github
	cd dist && sha256sum cast-$(VERSION)-linux-$$(uname -m).tar.gz cast-$(VERSION)-source.tar.gz > SHA256SUMS
release-check:
	sh packaging/release.sh check '$(VERSION)' '$(RELEASE_NOTES)' '$(X11)' '$(WAYLAND)' '$(RELEASE_TAG)' '$(PANEL)'
release:
	sh packaging/release.sh release '$(VERSION)' '$(RELEASE_NOTES)' '$(X11)' '$(WAYLAND)' '$(RELEASE_TAG)' '$(PANEL)'
release-ci:
	sh packaging/release.sh release-ci '$(VERSION)' '$(RELEASE_NOTES)' 1 1 '$(RELEASE_TAG)' 1
clean:
	rm -rf build cast
.PHONY: FORCE all check check-unit check-stream check-panel-routes check-wayland check-wayland-unit check-xorg check-loopback check-panel benchmark sanitize install uninstall package-check package release-check release-ci release clean
