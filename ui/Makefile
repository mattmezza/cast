# No CUI installation, Python, CMake, network access, or implementation macros.
.DEFAULT_GOAL := all
CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
# Consumer sources may live in subdirectories; target stays beside bundled assets.
APP_SOURCES ?= main.c app.c menus.c cast_theme.c brand.c settings.c source_picker.c
TARGET ?= build/cast-ui-preview
APP_OBJECTS := $(APP_SOURCES:%.c=build/%.o)
CPPFLAGS += -Ivendor/include -Ivendor/third_party/clay
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

# Check system prerequisites before compilation. CUI itself is in this bundle.
ifeq ($(shell $(PKG_CONFIG) --exists 'sdl3 >= 3.2' freetype2 harfbuzz && echo yes),)
$(error Install SDL3 >= 3.2, FreeType and HarfBuzz development packages; ensure pkg-config can find sdl3 freetype2 harfbuzz)
endif
NATIVE_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 freetype2 harfbuzz)
LDLIBS += $(shell $(PKG_CONFIG) --libs sdl3 freetype2 harfbuzz) -lm

include sources.mk
PORTABLE_OBJECTS := $(PORTABLE_SOURCES:%.c=build/%.o)
TEXT_OBJECTS := $(TEXT_SOURCES:%.c=build/%.o)
BIDI_OBJECTS := $(BIDI_SOURCES:%.c=build/%.o) $(SHEENBIDI_SOURCES:%.c=build/%.o)
SDL3_OBJECTS := $(SDL3_SOURCES:%.c=build/%.o)
OBJECTS := $(PORTABLE_OBJECTS) $(TEXT_OBJECTS) $(BIDI_OBJECTS) $(SDL3_OBJECTS)
LIBRARIES := build/vendor/libcui_sdl3.a build/vendor/libcui_text.a build/vendor/libcui_bidi.a build/vendor/libcui.a

all: $(TARGET)

$(TARGET): $(APP_OBJECTS) $(LIBRARIES)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/vendor/libcui.a: $(PORTABLE_OBJECTS)
	$(AR) rcs $@ $^
build/vendor/libcui_text.a: $(TEXT_OBJECTS)
	$(AR) rcs $@ $^
build/vendor/libcui_bidi.a: $(BIDI_OBJECTS)
	$(AR) rcs $@ $^
build/vendor/libcui_sdl3.a: $(SDL3_OBJECTS)
	$(AR) rcs $@ $^

$(TEXT_OBJECTS): CPPFLAGS += -DCUI_TEXT_BIDI
$(BIDI_OBJECTS): CPPFLAGS += -Ivendor/third_party/sheenbidi/Headers
$(SHEENBIDI_SOURCES:%.c=build/%.o): CPPFLAGS += -DSB_CONFIG_UNITY -Ivendor/third_party/sheenbidi/Source
# Narrow suppressions match CMake for unmodified pinned upstream SheenBidi.
$(SHEENBIDI_SOURCES:%.c=build/%.o): UPSTREAM_WARNINGS := -Wno-unused-parameter -Wno-unused-value -Wno-unused-function -Wno-implicit-fallthrough

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(NATIVE_CFLAGS) $(CFLAGS) $(UPSTREAM_WARNINGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d) $(APP_OBJECTS:.o=.d)
-include build/test_preview.d
build/test_preview: build/test_preview.o build/app.o build/menus.o build/cast_theme.o build/brand.o build/settings.o build/source_picker.o build/vendor/libcui.a
	$(CC) $(LDFLAGS) $^ -lm -o $@

check: build/test_preview
	./build/test_preview

check-native: $(TARGET)
	python3 test_native.py

clean:
	rm -rf build

.PHONY: all check check-native clean

# Transport stubs exercise privacy invalidation without SDL or a daemon.
build/test_bridge: test_bridge.c daemon_bridge.c daemon_bridge.h live.h ../src/panel_transport.h
	@mkdir -p build
	$(CC) -Ivendor/include -Ivendor/third_party/clay -I../src -std=gnu11 -Wall -Wextra -Werror test_bridge.c daemon_bridge.c -o $@
check-bridge: build/test_bridge
	./build/test_bridge
check: check-bridge
.PHONY: check-bridge
