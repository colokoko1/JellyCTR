# JellyCTR makefile v0.3.8
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

# Absolute pathing for tools
PREFIX  := $(DEVKITARM)/bin/arm-none-eabi-
CC      := $(PREFIX)gcc
CXX     := $(PREFIX)g++
LD      := $(PREFIX)g++

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      :=  JellyCTR
BUILD       :=  build
SOURCES     :=  source
DATA        :=  data
INCLUDES    :=  include source source/gfx/images
GRAPHICS    :=  source/gfx/images

# ARCH options
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

PORTLIBS := $(DEVKITPRO)/portlibs/3ds
LIBCTRU  := $(DEVKITPRO)/libctru

CFLAGS  :=  -g -Wall -O2 -mword-relocations \
            -ffunction-sections \
            $(ARCH)

CFLAGS  +=  -D__3DS__ \
            -I$(LIBCTRU)/include \
            -I$(PORTLIBS)/include \
            $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir))

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS :=  -g $(ARCH)
LDFLAGS =   -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBDIRS := $(PORTLIBS) $(LIBCTRU)
LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# Libraries
LIBS    := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljson-c -ljpeg -lctru -lstdc++ -lz -lm

# --- Main Build Logic ---

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
GFXFILES    :=  $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))

export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export T3XFILES := $(patsubst %.t3s,$(TOPDIR)/source/gfx/images/%.t3x,$(GFXFILES))

.PHONY: all clean

# The 'all' target now ensures graphics are built BEFORE entering the build directory
all: $(BUILD)
	@$(MAKE) $(T3XFILES) --no-print-directory
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo cleaning up build files...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TOPDIR)/source/gfx/images/*.t3x $(TOPDIR)/source/gfx/images/*.h

# Graphics generation rule moved to top-level to ensure availability
$(TOPDIR)/source/gfx/images/%.t3x: %.t3s
	@echo converting $(notdir $<)
	@tex3ds -i $< -H $(TOPDIR)/source/gfx/images/$*.h -o $@

else

# --- Sub-make Logic (Inside build/ folder) ---

$(OUTPUT).3dsx  :   $(OUTPUT).elf
$(OUTPUT).elf   :   $(OFILES)
	@echo linking $(notdir $@)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

# CRITICAL: This line tells the compiler that objects depend on the graphics headers
$(OFILES): $(T3XFILES)

%.o: %.cpp
	@echo $(notdir $<)
	@$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) -c $< -o $@

%.o: %.c
	@echo $(notdir $<)
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@

-include $(DEPSDIR)/*.d

endif