.POSIX:
CC = cc
PKG_CONFIG = pkg-config
PREFIX = /usr/local
DESTDIR =
X11 = 1
WAYLAND = 0
PANEL = 0
STT = 0
CXX = c++
STT_CFLAGS = $(shell pkg-config --cflags whisper)
STT_LIBS = $(shell pkg-config --libs --static whisper)
STT_CPU_BACKEND =
STT_HELPER_PATH = $(PREFIX)/libexec/cast-pro-stt-helper
STT_PROFILE = $(CURDIR)/build/deps-stt/profile.json
STT_ENGINEERING = 0
VERSION = 0.10.0
SOURCE_COMMIT = working-tree
RELEASE_NOTES =
RELEASE_TAG = v$(VERSION)
SUSTAINED_WIDTH = 1920
SUSTAINED_HEIGHT = 1080
LOOPBACK_DEVICE =
CFLAGS = -O2 -g
WARN = -Wall -Wextra -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes
BASE_PACKAGES = fontconfig freetype2 libavcodec libavformat libavutil libswscale libswresample libpipewire-0.3
PACKAGES = $(BASE_PACKAGES)
EDITION = community
MEDIA_PROFILE = system
PRO_ROOT = $(abspath ../cast-pro)
PRO_DIR = $(if $(wildcard $(PRO_ROOT)/pro/src/pro_provider.c),$(PRO_ROOT)/pro,$(PRO_ROOT))
PRO_TRUST_HEADER = $(PRO_DIR)/src/trusted_keys.h
LGPL_ROOT = $(CURDIR)/build/deps-lgpl
OFFICIAL_RELEASE = 0
RELEASE_TIMESTAMP = 0
PRIVATE_REVISION = $(if $(filter pro,$(EDITION)),$(shell git -C '$(PRO_ROOT)' rev-parse --verify HEAD 2>/dev/null || echo local-unversioned),not-linked)
CORE_REVISION = $(if $(filter-out working-tree,$(SOURCE_COMMIT)),$(SOURCE_COMMIT),$(shell git rev-parse --verify HEAD 2>/dev/null || echo working-tree))
ifeq ($(filter $(EDITION),community pro),)
$(error EDITION must be community or pro)
endif
ifeq ($(filter $(MEDIA_PROFILE),system lgpl),)
$(error MEDIA_PROFILE must be system or lgpl)
endif
ifneq ($(filter-out 0 1,$(X11) $(WAYLAND) $(PANEL) $(STT) $(STT_ENGINEERING)),)
$(error X11 WAYLAND PANEL STT STT_ENGINEERING must be 0 or 1)
endif
BINARY = cast
ifeq ($(EDITION),pro)
BINARY = cast-pro
ifneq ($(MEDIA_PROFILE),lgpl)
$(error Pro requires MEDIA_PROFILE=lgpl; system GPL media is forbidden)
endif
ifneq ($(filter /%,$(PRO_ROOT)),$(PRO_ROOT))
$(error PRO_ROOT must be an absolute private checkout path)
endif
ifeq ($(wildcard $(PRO_DIR)/src/pro_provider.c),)
$(error Pro requires a private provider under $(PRO_ROOT)/pro/src or $(PRO_ROOT)/src)
endif
PRO_SOURCES = $(PRO_DIR)/src/pro_provider.c $(PRO_DIR)/src/license_verifier.c $(PRO_DIR)/src/bounded_json.c $(wildcard $(PRO_DIR)/src/cinematic_motion/*.c $(PRO_DIR)/src/speech/*.c $(PRO_DIR)/src/notes/*.c)
CPPFLAGS += -DCAST_EDITION_PRO=1 -DWITH_PRO -I$(PRO_DIR)/src -DCAST_TRUST_HEADER='"$(PRO_TRUST_HEADER)"'
LDLIBS += $(shell $(PKG_CONFIG) --libs libsodium)
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libsodium)
endif
ifeq ($(MEDIA_PROFILE),lgpl)
CPPFLAGS += -DCAST_MEDIA_PROFILE_LGPL=1
PKG_CONFIG = env PKG_CONFIG_PATH=$(LGPL_ROOT)/prefix/lib/pkgconfig pkg-config
override LDFLAGS += -Wl,-rpath,'$$ORIGIN/../lib/$(BINARY)/media'
ifeq ($(OFFICIAL_RELEASE),0)
# Local SDK lookup is restricted to visibly nonproduction development binaries.
override LDFLAGS += -Wl,-rpath,$(LGPL_ROOT)/prefix/lib
endif
endif
CPPFLAGS += -DCAST_APPLICATION_NAME='"$(BINARY)"' -DCAST_MEDIA_PROFILE='"$(MEDIA_PROFILE)"' -DCAST_PLATFORM='"linux-$(shell uname -m)"' -DCAST_CORE_REVISION='"$(CORE_REVISION)"' -DCAST_PRIVATE_REVISION='"$(PRIVATE_REVISION)"' -DCAST_RELEASE_TIMESTAMP=$(RELEASE_TIMESTAMP) -DCAST_OFFICIAL_RELEASE=$(OFFICIAL_RELEASE)
SOURCES = src/main.c src/commands.c src/config.c src/config_migrate.c src/state.c src/compositor.c src/composition_assets.c src/presentation_text.c src/platform.c src/media.c src/webcam.c src/audio.c src/record.c src/stream.c src/panel_transport.c src/panel_lifecycle.c src/help_commands.c src/update.c src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c src/edition_service.c src/media_codec.c src/app_runtime.c vendor/inih/ini.c
INI_FLAGS = -DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 -DINI_ALLOW_MULTILINE=0 -DINI_ALLOW_INLINE_COMMENTS=0 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=8192
CPPFLAGS += -Isrc -Ivendor/inih $(INI_FLAGS) -D_GNU_SOURCE
LDLIBS += -lm -lpthread -ldl
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
BUILD = build/$(EDITION)-x$(X11)-w$(WAYLAND)-p$(PANEL)-$(MEDIA_PROFILE)-$(INTEGRATION_ID)
COMMAND_ASSETS = $(BUILD)/src/command_assets.o
FONT_OBJECT =
ifneq ($(filter 1,$(X11) $(PANEL)),)
FONT_OBJECT = $(BUILD)/src/panel_font.o
endif
# Hash every provider/trust identity as well as backend/media options into object paths.
STT_IDENTITY = $(if $(filter pro:1,$(EDITION):$(STT)),$(STT_CFLAGS):$(STT_LIBS):$(STT_CPU_BACKEND):$(STT_HELPER_PATH):$(STT_PROFILE):$(STT_ENGINEERING):$(shell test ! -f '$(STT_PROFILE)' || sha256sum '$(STT_PROFILE)'),disabled)
INTEGRATION_ID := $(shell { printf "%s" "$(EDITION):$(X11):$(WAYLAND):$(PANEL):$(STT):$(STT_IDENTITY):$(MEDIA_PROFILE):$(LGPL_ROOT):$(PRO_ROOT):$(CORE_REVISION):$(PRIVATE_REVISION):$(OFFICIAL_RELEASE):$(RELEASE_TIMESTAMP):$(CC):$(CFLAGS):$(LDFLAGS):$(PANEL_CFLAGS):$(PANEL_LIBS)"; cat $(wildcard src/*.c src/*.h src/*.S vendor/inih/*.c vendor/inih/*.h vendor/clay/*.h completions/* assets/fonts/* packaging/install.sh) $(if $(filter pro,$(EDITION)),$(wildcard $(PRO_DIR)/src/*.c $(PRO_DIR)/src/*.h $(PRO_DIR)/src/*/*.c $(PRO_DIR)/src/*/*.h $(PRO_DIR)/src/*/*.cpp $(PRO_TRUST_HEADER))) 2>/dev/null; } | sha256sum | cut -c1-32)
CPPFLAGS += -DCAST_BUILD_ID='"$(INTEGRATION_ID)"'
SOURCES += $(PRO_SOURCES)
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
SOURCES += src/panel.c src/panel_color_pick.c
CPPFLAGS += -DWITH_PANEL -Ivendor/clay $(PANEL_CFLAGS)
LDLIBS += $(PANEL_LIBS)


endif
CORE_SOURCES = src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c $(PRO_SOURCES) src/compositor.c src/composition_assets.c src/presentation_text.c src/config.c src/state.c vendor/inih/ini.c
MEDIA_SOURCES = src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c $(PRO_SOURCES) src/media.c src/webcam.c src/audio.c src/record.c src/stream.c src/compositor.c src/composition_assets.c src/presentation_text.c src/media_codec.c

ifeq ($(EDITION):$(STT),pro:1)
CPPFLAGS += -DCAST_WITH_STT -DCAST_STT_HELPER_PATH='"$(STT_HELPER_PATH)"'
STT_HELPER = $(BUILD)/cast-pro-stt-helper
endif

all: $(BINARY) $(STT_HELPER)
FORCE:
$(BINARY): FORCE $(OBJECTS) $(STT_HELPER)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) $(LDLIBS)
ifeq ($(EDITION),pro)
	@env LD_LIBRARY_PATH='$(LGPL_ROOT)/prefix/lib' python3 -c 'import json,subprocess,sys; i=json.loads(subprocess.check_output(["./cast-pro","edition","--json"])); sys.exit(0 if i.get("edition")=="pro" and i.get("media_profile")=="lgpl" and i.get("extension_api")==2 else 1)' || { echo 'Pro provider/API/edition verification failed after link; no package/install permitted' >&2; rm -f cast-pro; exit 1; }
endif
$(OBJECTS): | edition-preflight
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
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_core.c $(CORE_SOURCES) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_visual: tests/test_visual.c tests/compositor_runtime_stubs.c src/compositor.c src/composition_assets.c src/presentation_text.c src/cast.h src/compositor.h src/pro_runtime.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_media: tests/test_media.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_stream: tests/test_stream.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h src/stream.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
check-stream-sustained: $(BUILD)/test_stream
	python3 tests/test_stream_sustained.py --binary $(BUILD)/test_stream
check-stream: $(BUILD)/test_stream
	$(BUILD)/test_stream
	python3 tests/test_stream_network.py --binary $(BUILD)/test_stream

$(BUILD)/test_wayland: tests/test_wayland.c tests/compositor_runtime_stubs.c src/wayland.c src/compositor.c src/composition_assets.c src/presentation_text.c src/state.c src/cast.h src/compositor.h src/pro_runtime.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_wayland.c tests/compositor_runtime_stubs.c src/compositor.c src/composition_assets.c src/presentation_text.c src/state.c $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_commands: tests/test_commands.c $(SOURCES) src/app_internal.h src/cast.h src/media_internal.h src/platform_backend.h $(COMMAND_ASSETS) $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_commands.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(COMMAND_ASSETS) $(FONT_OBJECT) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_panel_transport: tests/test_panel_transport.c $(SOURCES) src/app_internal.h src/panel_transport.h $(COMMAND_ASSETS) $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_panel_transport.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(COMMAND_ASSETS) $(FONT_OBJECT) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
ifeq ($(PANEL),1)
$(BUILD)/test_panel_routes: tests/test_panel_routes.c $(SOURCES) src/panel_transport.h $(BUILD)/src/panel_font.o $(COMMAND_ASSETS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_panel_routes.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(BUILD)/src/panel_font.o $(COMMAND_ASSETS) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_panel_color_pick: tests/test_panel_color_pick.c src/panel_color_pick.c src/panel_color_pick.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ tests/test_panel_color_pick.c src/panel_color_pick.c $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
check-panel-color-pick: $(BUILD)/test_panel_color_pick
	$(BUILD)/test_panel_color_pick
check-unit: check-panel-routes check-panel-color-pick
check-panel-routes: $(BUILD)/test_panel_routes
	$(BUILD)/test_panel_routes
endif
$(BUILD)/benchmark: tests/benchmark.c $(CORE_SOURCES) src/cast.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
benchmark: $(BUILD)/benchmark
	$(BUILD)/benchmark
ifeq ($(X11),1)
XORG_TEST_SOURCES = tests/x11_smoke.c tests/compositor_runtime_stubs.c src/compositor.c src/composition_assets.c src/presentation_text.c src/platform.c src/x11.c src/preview_text.c
ifeq ($(WAYLAND),1)
XORG_TEST_SOURCES += src/wayland.c
endif
$(BUILD)/test_xorg: $(XORG_TEST_SOURCES) src/cast.h src/platform_backend.h $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(shell $(PKG_CONFIG) --cflags xtst) $(CFLAGS) $(WARN) -std=gnu11 -o $@ $(XORG_TEST_SOURCES) $(FONT_OBJECT) $(PKG_LIBS) $(shell $(PKG_CONFIG) --libs xtst) $(LDLIBS) $(LDFLAGS)
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
ifeq ($(EDITION),community)
check: $(BINARY) check-unit
	python3 tests/test_ipc.py
	python3 tests/test_output_modes.py
	python3 tests/test_three_outputs.py
	python3 tests/test_presentation_layers.py
	python3 tests/test_help_commands.py
	python3 tests/test_install.py
	python3 tests/test_app_install.py
else
check: $(BINARY) check-unit check-pro-cli check-update-offline
endif
check-loopback: $(BINARY)
	@test -n '$(LOOPBACK_DEVICE)' || { echo 'set LOOPBACK_DEVICE to an existing v4l2loopback output device' >&2; exit 1; }
	python3 tests/test_loopback.py --device '$(LOOPBACK_DEVICE)'
ifeq ($(PANEL),1)
check-panel: cast
	python3 tests/test_panel_ui.py
ifeq ($(X11),1)
	python3 tests/test_panel_ui.py --exclusion-only
endif
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

install: $(BINARY) $(STT_HELPER)
	install -Dm755 $(BINARY) $(DESTDIR)$(PREFIX)/bin/$(BINARY)
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/LICENSE
	install -Dm644 vendor/inih/LICENSE.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/inih-LICENSE
	install -Dm644 licenses/GPL-3.0.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/GPL-3.0.txt
	install -Dm644 licenses/LGPL-3.0.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/LGPL-3.0.txt
	install -Dm644 licenses/LGPL-2.1.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/LGPL-2.1.txt
	install -Dm644 licenses/FreeType-FTL.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/FreeType-FTL
	install -Dm644 licenses/Fontconfig-COPYING.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/Fontconfig-COPYING
	install -Dm644 docs/cast.1 $(DESTDIR)$(PREFIX)/share/man/man1/$(BINARY).1
	install -Dm644 examples/cast.conf $(DESTDIR)$(PREFIX)/share/doc/$(BINARY)/$(BINARY).conf.example
	install -Dm644 examples/sxhkdrc $(DESTDIR)$(PREFIX)/share/doc/$(BINARY)/sxhkdrc.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/$(BINARY)/README.md
	install -Dm644 completions/cast.bash $(DESTDIR)$(PREFIX)/share/bash-completion/completions/$(BINARY)
	install -Dm644 completions/_cast $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_$(BINARY)
	install -Dm644 completions/cast.fish $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/$(BINARY).fish
ifeq ($(PANEL),1)
	ln -sfn $(BINARY) $(DESTDIR)$(PREFIX)/bin/$(BINARY)-app
	install -Dm644 packaging/$(BINARY).desktop $(DESTDIR)$(PREFIX)/share/applications/$(BINARY).desktop
	install -Dm644 assets/cast.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(BINARY).svg
	install -Dm644 vendor/clay/LICENSE.md $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/clay-LICENSE
	install -Dm644 licenses/SDL3_ttf-ZLIB.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/SDL3_ttf-ZLIB
endif
ifneq ($(filter 1,$(X11) $(PANEL)),)
	install -Dm644 assets/fonts/OFL.txt $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)/Inter-OFL
endif
	@for doc in docs/*.md; do install -Dm644 "$$doc" "$(DESTDIR)$(PREFIX)/share/doc/$(BINARY)/$${doc##*/}"; done
ifeq ($(EDITION),pro)
	install -Dm644 $(PRO_DIR)/LICENSE $(DESTDIR)$(PREFIX)/share/licenses/cast-pro/PRO-LICENSE
	install -Dm644 $(PRO_DIR)/EULA-DRAFT.md $(DESTDIR)$(PREFIX)/share/licenses/cast-pro/EULA-DRAFT.md
	install -d $(DESTDIR)$(PREFIX)/lib/cast-pro/media
	cp -a $(LGPL_ROOT)/prefix/lib/*.so* $(DESTDIR)$(PREFIX)/lib/cast-pro/media/
	sed 's/cast/cast-pro/g' completions/cast.bash > $(DESTDIR)$(PREFIX)/share/bash-completion/completions/cast-pro
	sed 's/cast/cast-pro/g' completions/_cast > $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_cast-pro
	sed 's/cast/cast-pro/g' completions/cast.fish > $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/cast-pro.fish
ifeq ($(STT),1)
	install -Dm755 $(STT_HELPER) $(DESTDIR)$(PREFIX)/libexec/cast-pro-stt-helper
	install -Dm644 '$(STT_PROFILE)' $(DESTDIR)$(PREFIX)/share/doc/cast-pro/speech-profile.json
	install -Dm644 '$(dir $(STT_PROFILE))THIRD-PARTY-NOTICES.txt' $(DESTDIR)$(PREFIX)/share/doc/cast-pro/speech-SDK-NOTICES.txt
	mkdir -p $(DESTDIR)$(PREFIX)/share/doc/cast-pro/speech-license-texts
	cp -R '$(dir $(STT_PROFILE))license-texts/.' $(DESTDIR)$(PREFIX)/share/doc/cast-pro/speech-license-texts/
endif
	@for doc in $(PRO_DIR)/docs/*.md; do test ! -f "$$doc" || install -Dm644 "$$doc" "$(DESTDIR)$(PREFIX)/share/doc/cast-pro/$${doc##*/}"; done
endif
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BINARY) $(DESTDIR)$(PREFIX)/bin/$(BINARY)-app $(DESTDIR)$(PREFIX)/share/man/man1/$(BINARY).1
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(BINARY).desktop $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(BINARY).svg
	rm -f $(DESTDIR)$(PREFIX)/share/bash-completion/completions/$(BINARY) $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_$(BINARY) $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/$(BINARY).fish
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/$(BINARY) $(DESTDIR)$(PREFIX)/share/licenses/$(BINARY)
ifeq ($(EDITION),pro)
	rm -rf $(DESTDIR)$(PREFIX)/lib/cast-pro
	rm -f $(DESTDIR)$(PREFIX)/libexec/cast-pro-stt-helper
endif
package-check:
	@test '$(VERSION)' = "$$(sed -n 's/^#define CAST_VERSION "\([^"]*\)"/\1/p' src/cast.h)" || { echo 'VERSION must match CAST_VERSION in src/cast.h' >&2; exit 1; }
package: package-check $(BINARY) check-public-boundary
	python3 packaging/package-edition.py --edition $(EDITION) --binary $(BINARY) --version $(VERSION) --media-profile $(MEDIA_PROFILE) --pro-root '$(PRO_ROOT)' --lgpl-root '$(LGPL_ROOT)' --x11 $(X11) --wayland $(WAYLAND) --panel $(PANEL) --official $(OFFICIAL_RELEASE) --stt $(STT) --trust-header '$(PRO_TRUST_HEADER)' --stt-helper-path '$(STT_HELPER_PATH)' --stt-cpu-backend '$(STT_CPU_BACKEND)' --stt-profile '$(STT_PROFILE)' --stt-engineering $(STT_ENGINEERING)
package-pro:
	$(MAKE) EDITION=pro MEDIA_PROFILE=lgpl package
install-community:
	$(MAKE) EDITION=community install
install-pro:
	$(MAKE) EDITION=pro MEDIA_PROFILE=lgpl install
edition-preflight:
	python3 packaging/audit.py profile --edition $(EDITION) --media-profile $(MEDIA_PROFILE) --lgpl-root '$(LGPL_ROOT)' --pro-root '$(PRO_ROOT)' --trust-header '$(PRO_TRUST_HEADER)' --official $(OFFICIAL_RELEASE) --release-timestamp $(RELEASE_TIMESTAMP)
ifeq ($(EDITION):$(STT),pro:1)
	python3 packaging/speech-profile.py verify --profile '$(STT_PROFILE)' --official $(OFFICIAL_RELEASE) --engineering $(STT_ENGINEERING) --cpu-backend '$(STT_CPU_BACKEND)' --cflags='$(STT_CFLAGS)' --libs='$(STT_LIBS)'
endif
deps-lgpl:
	sh tools/deps-lgpl.sh $(DEPS_ARGS)
check-public-boundary:
	python3 packaging/public-boundary.py
check-licenses: $(BINARY)
	python3 packaging/audit.py inventory --binary $(BINARY) --edition $(EDITION) --media-profile $(MEDIA_PROFILE) --lgpl-root '$(LGPL_ROOT)' --pro-root '$(PRO_ROOT)' --official $(OFFICIAL_RELEASE) --output '$(BUILD)/audit' --strict
check-editions: check-public-boundary
check-runtime-acceptance:
	python3 tests/test_runtime_acceptance.py
check-editions: check-runtime-acceptance
.PHONY: check-runtime-acceptance
release-check:
	@test '$(EDITION)' = community || { echo 'Pro uses private signed release staging; no public release channel' >&2; exit 1; }
	sh packaging/release.sh check '$(VERSION)' '$(RELEASE_NOTES)' '$(X11)' '$(WAYLAND)' '$(RELEASE_TAG)' '$(PANEL)'
release:
	@test '$(EDITION)' = community || { echo 'Pro uses private signed release staging; no public release channel' >&2; exit 1; }
	sh packaging/release.sh release '$(VERSION)' '$(RELEASE_NOTES)' '$(X11)' '$(WAYLAND)' '$(RELEASE_TAG)' '$(PANEL)'
release-ci:
	@test '$(EDITION)' = community || { echo 'Pro uses private signed release staging; no public release channel' >&2; exit 1; }
	sh packaging/release.sh release-ci '$(VERSION)' '$(RELEASE_NOTES)' 1 1 '$(RELEASE_TAG)' 1
clean:
	rm -rf build cast cast-pro
.PHONY: FORCE all check check-unit check-stream check-stream-sustained check-panel-routes check-wayland check-wayland-unit check-xorg check-loopback check-panel benchmark sanitize install uninstall package-check package release-check release-ci release clean

.PHONY: edition-preflight deps-lgpl check-public-boundary check-licenses check-editions package-pro install-community install-pro

$(BUILD)/media_sustained: tests/media_sustained.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
check-media-sustained: $(BUILD)/media_sustained
	@test '$(MEDIA_PROFILE)' = lgpl || { echo 'check-media-sustained requires MEDIA_PROFILE=lgpl' >&2; exit 1; }
	$(BUILD)/media_sustained "$(BUILD)/sustained-$$(date +%s).mkv" $(SUSTAINED_WIDTH) $(SUSTAINED_HEIGHT)
.PHONY: check-media-sustained
$(BUILD)/test_media_quality: tests/test_media_quality.c $(MEDIA_SOURCES) src/config.c vendor/inih/ini.c src/cast.h src/media_internal.h src/media_codec.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -std=gnu11 -DCAST_TEST -o $@ $(filter %.c,$^) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
check-media-quality: $(BUILD)/test_media_quality
	$(BUILD)/test_media_quality "$(BUILD)/quality-$$(date +%s).mkv"
.PHONY: check-media-quality
check-static-deps:
	python3 tools/check-static-deps.py --edition $(EDITION) --lgpl-root '$(LGPL_ROOT)' --x11 $(X11) --wayland $(WAYLAND) --panel $(PANEL) --stt $(STT)
.PHONY: check-static-deps

check-update-offline:
	python3 tests/test_update_offline.py --pro-root '$(PRO_DIR)'
.PHONY: check-update-offline

check-pro-cli: $(BINARY)
	@test '$(EDITION)' = pro || { echo 'check-pro-cli requires explicit Pro test build' >&2; exit 1; }
	python3 tests/test_pro_cli.py --binary '$(BINARY)' --pro-root '$(PRO_DIR)'

ifeq ($(EDITION),pro)
$(BUILD)/test_pro_workflows: $(PRO_DIR)/tests/test_core_workflows.c $(SOURCES) $(COMMAND_ASSETS) $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WARN) -Werror -std=gnu11 -DCAST_CORE_MAIN='"$(CURDIR)/src/main.c"' -o $@ $(PRO_DIR)/tests/test_core_workflows.c $(filter-out src/main.c src/panel.c,$(SOURCES)) $(COMMAND_ASSETS) $(FONT_OBJECT) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
$(BUILD)/test_pro_workflows: | edition-preflight
check-pro-workflows: $(BUILD)/test_pro_workflows
	@test -n '$(PRO_TEST_LICENSE)' || { echo 'set PRO_TEST_LICENSE to a nonproduction signed fixture' >&2; exit 1; }
	$(BUILD)/test_pro_workflows '$(PRO_TEST_LICENSE)'
endif
.PHONY: check-pro-workflows
.PHONY: check-pro-cli

check-edition-install:
	python3 tests/test_edition_install.py --pro-root '$(PRO_ROOT)' --lgpl-root '$(LGPL_ROOT)'
.PHONY: check-edition-install

build/edition-contract-tests/test_editions: tests/test_editions.c src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c src/edition.h src/license_store.h src/edition_extensions.h src/pro_extension.h src/ipc_identity.h src/cast.h
	@mkdir -p build/edition-contract-tests
	$(CC) -D_GNU_SOURCE -Isrc -std=gnu11 -O2 $(WARN) -Werror -o $@ $(filter %.c,$^)
build/edition-contract-tests/test_editions_contract: tests/test_editions_contract.c src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c src/config.c src/presentation_text.c vendor/inih/ini.c src/edition.h src/license_store.h src/edition_extensions.h src/pro_extension.h src/presentation_text.h src/cast.h
	@mkdir -p build/edition-contract-tests
	$(CC) -D_GNU_SOURCE -DWITH_PRO -Isrc -Ivendor/inih $(INI_FLAGS) $(shell pkg-config --cflags fontconfig freetype2) -std=gnu11 -O2 -ffunction-sections -fdata-sections $(WARN) -Werror -o $@ $(filter %.c,$^) -Wl,--gc-sections $(shell pkg-config --libs fontconfig freetype2) -lm
build/edition-contract-tests/test_edition_service: tests/test_edition_service.c src/edition_service.c src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c src/config.c src/presentation_text.c vendor/inih/ini.c src/edition_service.h src/edition.h src/license_store.h src/edition_extensions.h src/pro_extension.h src/presentation_text.h src/cast.h
	@mkdir -p build/edition-contract-tests
	$(CC) -D_GNU_SOURCE -Isrc -Ivendor/inih $(INI_FLAGS) $(shell pkg-config --cflags fontconfig freetype2) -std=gnu11 -O2 -ffunction-sections -fdata-sections $(WARN) -Werror -o $@ $(filter %.c,$^) -Wl,--gc-sections $(shell pkg-config --libs fontconfig freetype2) -lpthread -lm
check-edition-service: build/edition-contract-tests/test_edition_service
	build/edition-contract-tests/test_edition_service
build/edition-contract-tests/test_workflow_service: tests/test_workflow_service.c src/edition_service.c src/edition.c src/license_store.c src/edition_extensions.c src/workflow_schema.c src/config.c src/presentation_text.c vendor/inih/ini.c src/pro_runtime.h src/pro_extension.h src/edition_service.h src/cast.h
	@mkdir -p build/edition-contract-tests
	$(CC) -D_GNU_SOURCE -DWITH_PRO -Isrc -Ivendor/inih $(INI_FLAGS) $(shell pkg-config --cflags fontconfig freetype2) -std=gnu11 -O2 -ffunction-sections -fdata-sections $(WARN) -Werror -o $@ $(filter %.c,$^) -Wl,--gc-sections $(shell pkg-config --libs fontconfig freetype2) -lpthread -lm
check-workflow-service: build/edition-contract-tests/test_workflow_service
	build/edition-contract-tests/test_workflow_service
$(BUILD)/test_workflow_runtime: tests/test_workflow_runtime.c $(SOURCES) src/app_internal.h src/pro_runtime.h src/pro_extension.h $(COMMAND_ASSETS) $(FONT_OBJECT)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DWITH_PRO $(PKG_CFLAGS) $(CFLAGS) $(WARN) -Werror -std=gnu11 -o $@ tests/test_workflow_runtime.c $(filter-out src/main.c src/panel.c $(PRO_SOURCES),$(SOURCES)) $(COMMAND_ASSETS) $(FONT_OBJECT) $(PKG_LIBS) $(LDLIBS) $(LDFLAGS)
check-workflow-runtime: $(BUILD)/test_workflow_runtime
	$(BUILD)/test_workflow_runtime
.PHONY: check-edition-service
check-split-cli: $(BINARY)
	python3 tests/test_split_cli.py --binary '$(BINARY)'
check: check-split-cli
.PHONY: check-split-cli
check-editions: build/edition-contract-tests/test_editions build/edition-contract-tests/test_editions_contract check-edition-service check-workflow-service
	python3 tests/test_editions_build.py
	build/edition-contract-tests/test_editions
	build/edition-contract-tests/test_editions_contract

ifeq ($(EDITION):$(STT),pro:1)
STT_IDENTITY_SOURCES = src/edition.c src/license_store.c $(PRO_DIR)/src/license_verifier.c $(PRO_DIR)/src/bounded_json.c
STT_IDENTITY_OBJECTS = $(STT_IDENTITY_SOURCES:%.c=$(BUILD)/helper/%.o)
$(STT_HELPER) $(STT_IDENTITY_OBJECTS) $(BUILD)/helper/whisper_helper.o: | edition-preflight
$(BUILD)/helper/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Werror -ffunction-sections -fdata-sections -std=gnu11 -c $< -o $@
$(BUILD)/helper/whisper_helper.o: $(PRO_DIR)/src/speech/whisper_helper.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(STT_CFLAGS) -DCAST_SPEECH_HELPER_EXECUTABLE $(if $(STT_CPU_BACKEND),-DCAST_GGML_CPU_BACKEND_PATH='"$(STT_CPU_BACKEND)"') $(CFLAGS) -Wall -Wextra -Werror -std=c++17 -ffunction-sections -fdata-sections -c $< -o $@
$(STT_HELPER): $(BUILD)/helper/whisper_helper.o $(STT_IDENTITY_OBJECTS)
	$(CXX) $(LDFLAGS) -Wl,--gc-sections -o $@ $^ $(STT_LIBS) $(shell pkg-config --libs libsodium) -lpthread -lm
endif

$(BUILD)/test_panel_command_queue: tests/test_panel_command_queue.c src/panel_transport.c src/ipc_identity.h src/edition.c src/commands.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Werror -std=gnu11 -ffunction-sections -fdata-sections -o $@ tests/test_panel_command_queue.c src/edition.c src/commands.c -Wl,--gc-sections -pthread -lm $(LDFLAGS)
check-panel-command-queue: $(BUILD)/test_panel_command_queue
	$(BUILD)/test_panel_command_queue
.PHONY: check-panel-command-queue

check-unit: check-workflow-runtime check-panel-command-queue
