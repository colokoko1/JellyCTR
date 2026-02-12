#---------------------------------------------------------------------------------
# JellyCTR Makefile - Fixed
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

include $(DEVKITARM)/3ds_rules

TARGET      :=  $(notdir $(CURDIR))
BUILD       :=  build
SOURCES     :=  source
DATA        :=  data

# LIBRARIES
LIBS := -lcitro2d -lcitro3d -lcurl -ljson-c -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthread -lctru -lm
# BUILD CHANNELS
LIBDIRS :=  $(CTRULIB) $(DEVKITPRO)/portlibs/3ds
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# COMPILER FLAGS
CFLAGS  :=  -g -Wall -O2 -mword-relocations \
            -fomit-frame-pointer -ffunction-sections \
            $(ARCH)

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS :=  -g $(ARCH)
LDFLAGS =   -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Rules for the build
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export OFILES   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

# FIXED INCLUDE LOGIC
export INCLUDE_FLAGS := -I$(CURDIR)/source \
                        -I$(CURDIR)/include \
                        $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                        -I$(DEVKITPRO)/portlibs/3ds/include

export LIBPATH := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

else

$(OUTPUT).3dsx  :   $(OUTPUT).elf
$(OUTPUT).elf   :   $(OFILES)
	@echo linking $(notdir $@)
	@$(CXX) $(LDFLAGS) $(OFILES) $(LIBPATH) $(LIBS) -o $@

%.o :   %.cpp
	@echo $(notdir $<)
	@$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) $(INCLUDE_FLAGS) -c $< -o $@

%.o :   %.c
	@echo $(notdir $<)
	@$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) $(INCLUDE_FLAGS) -c $< -o $@

endif