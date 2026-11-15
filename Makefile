ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro 3DS development tools first.)
endif

include $(DEVKITARM)/3ds_rules

TARGET      := ShieldDJStandard
BUILD       := build
SOURCES     := source
INCLUDES    := include
DATA        :=
GRAPHICS    :=
ROMFS       :=

APP_VERSION     := v1.0.36
APP_CIA_VERSION := 1060
APP_TITLE       := Shield DJ Standard by Shield-77
APP_DESCRIPTION := Lecteur MP3 et OGG avec pochette et interface avancee
APP_AUTHOR      := Shield-77
APP_PRODUCT_CODE := SHLD3DS03
APP_UNIQUE_ID   := 0xA2103
MAKEROM         ?= makerom

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections -fdata-sections $(ARCH)
CFLAGS   += -I$(DEVKITPRO)/libctru/include -I$(DEVKITPRO)/portlibs/3ds/include -Iinclude -D__3DS__ -D_POSIX_C_SOURCE=200809L -DMINIMP3_NO_SIMD
CXXFLAGS := $(CFLAGS) -std=gnu++11 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS     := -L$(DEVKITPRO)/libctru/lib -L$(DEVKITPRO)/portlibs/3ds/lib -lcitro2d -lcitro3d -lctru -lvorbisidec -logg -ljpeg -lpng -lz -lm

.PHONY: all cia clean
all: cia

cia: $(BUILD)/$(TARGET).cia

$(BUILD)/$(TARGET).cia: $(BUILD)/$(TARGET).elf Makefile app.rsf icon.png banner.png banner.wav
	@mkdir -p $(BUILD)
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i icon.png -o $(BUILD)/icon.icn
	@bannertool makebanner -i banner.png -a banner.wav -o $(BUILD)/banner.bnr
	@"$(MAKEROM)" -f cia -o "$@.tmp" -target t -ver $(APP_CIA_VERSION) -desc app:4 -rsf app.rsf -exefslogo -elf $(BUILD)/$(TARGET).elf -icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr
	@mv -f "$@.tmp" "$@"

$(BUILD)/$(TARGET).elf: $(SOURCES)/main.c $(SOURCES)/top_font.S assets/fonts/ShieldUISans40-Semibold.bcfnt include/ui_assets.h include/minimp3.h include/shield_ui_language.h $(DEVKITPRO)/portlibs/3ds/include/tremor/ivorbisfile.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -Iinclude -o $@ $(SOURCES)/main.c $(SOURCES)/top_font.S $(LIBS)

clean:
	@rm -rf $(BUILD)
