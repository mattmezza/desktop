CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/desktop
VERSION ?= 0.1.4
BUILD ?= build
PACKAGES = x11 xrandr xrender xfixes xext xscrnsaver freetype2 harfbuzz libpng libjpeg libwebp

CPPFLAGS += -D_GNU_SOURCE -DCUI_HAS_TEXT=1 -Ivendor/cui/include -Ivendor/cui/third_party/clay \
            $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
ifeq ($(STATIC),1)
LDFLAGS += -static
LDLIBS += -Wl,--start-group $(shell $(PKG_CONFIG) --static --libs $(PACKAGES)) -lstdc++ -pthread -lm -Wl,--end-group
else
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -pthread -lm
endif

APP = $(patsubst %.c,$(BUILD)/%.o,$(wildcard src/*.c))
CUI = $(addprefix $(BUILD)/vendor/cui/,src/core/cui.o src/core/cui_background.o \
       src/effects/cui_effects.o src/text/cui_text.o backends/software/cui_software.o \
       src/base/cui_base.o src/base/cui_edit.o src/base/cui_icon.o src/base/cui_content.o src/base/cui_models.o \
       src/base/cui_disclosure.o \
       src/components/cui_components.o src/components/cui_content.o src/theme/cui_theme.o src/theme/cui_theme_motion.o)
OBJECTS = $(APP) $(CUI) $(BUILD)/assets/embedded.o
WARNINGS = -Wall -Wextra -Wpedantic

.PHONY: all check clean install release
ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif
all: $(BUILD)/desktop

$(BUILD)/desktop: $(OBJECTS) Makefile
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD)/src/main.o: CPPFLAGS += -DDESKTOP_VERSION='"$(VERSION)"'
$(BUILD)/src/%.o: src/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD)/vendor/%.o: vendor/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/assets/embedded.o: assets/embedded.S assets/icons.alpha $(wildcard assets/*.ttf) config.ini
	@mkdir -p $(@D)
	$(CC) -c $< -o $@

-include $(OBJECTS:.o=.d)

check: all
	xvfb-run -a -s '-noreset -screen 0 1440x900x24' python3 tests/x11.py "$(BUILD)/desktop"

install: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(DATADIR)" "$(DESTDIR)$(PREFIX)/share/applications" "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps"
	install -m 755 "$(BUILD)/desktop" "$(DESTDIR)$(BINDIR)/desktop"
	install -m 644 config.ini LICENSE assets/*-LICENSE "$(DESTDIR)$(DATADIR)/"
	install -m 644 vendor/cui/LICENSE "$(DESTDIR)$(DATADIR)/CUI-LICENSE"
	install -m 644 vendor/cui/third_party/clay/LICENSE "$(DESTDIR)$(DATADIR)/CLAY-LICENSE"
	install -m 644 vendor/cui/third_party/nanosvg/LICENSE "$(DESTDIR)$(DATADIR)/NANOSVG-LICENSE"
	install -m 644 assets/io.github.mattmezza.desktop.desktop "$(DESTDIR)$(PREFIX)/share/applications/"
	install -m 644 assets/io.github.mattmezza.desktop.svg "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/"

clean:
	rm -rf "$(BUILD)"

release: export name := $(name)
release:
	sh tools/release.sh
