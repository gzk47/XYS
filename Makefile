#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)

#---------------------------------------------------------------------------------
# TARGET      output name
# BUILD       intermediate files
# SOURCES     directories with source files
# DATA        directories with binary data
# INCLUDES    directories with headers
# ROMFS       directory packed into romfs
#---------------------------------------------------------------------------------
TARGET      := xys
BUILD       := build
SOURCES     := source
DATA        :=
INCLUDES    := source
ROMFS       := romfs

APP_TITLE   := XYS
APP_AUTHOR  := gzk_47
APP_VERSION := 1.0.1

DEFINES     := -DAPP_AUTHOR=\"$(APP_AUTHOR)\" -DAPP_VERSION=\"$(APP_VERSION)\"

export APP_TITLE APP_AUTHOR APP_VERSION

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# SDL2 provides the graphical UI. Install switch-sdl2, switch-sdl2_image,
# switch-sdl2_ttf and switch-freetype.
#---------------------------------------------------------------------------------
SDL_CFLAGS  := $(shell $(PREFIX)pkg-config --cflags sdl2 SDL2_image SDL2_ttf 2>/dev/null)
SDL_LIBS    := $(shell $(PREFIX)pkg-config --static --libs SDL2_ttf SDL2_image sdl2 2>/dev/null)

# Fall back to explicit flags when the pkg-config wrapper is unavailable.
ifeq ($(strip $(SDL_LIBS)),)
SDL_CFLAGS  := -I$(PORTLIBS)/include/SDL2
SDL_LIBS    := -lSDL2_ttf -lSDL2_image -lSDL2 -lfreetype -lpng -ljpeg -lz \
               -lEGL -lglapi -ldrm_nouveau
endif

#---------------------------------------------------------------------------------
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS      := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) $(SDL_CFLAGS) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS     := -g $(ARCH)
LDFLAGS     = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS        := $(SDL_LIBS) -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx

LIBDIRS     := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES        := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES        := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD       := $(CC)
else
export LD       := $(CXX)
endif

export OFILES   := $(addsuffix .o,$(BINFILES)) \
                   $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS) --nacp=$(TARGET).nacp \
                   --icon=$(CURDIR)/romfs/icon.jpg

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nro    : $(OUTPUT).elf $(TARGET).nacp
$(OUTPUT).elf    : $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
