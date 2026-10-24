.RECIPEPREFIX := >

TARGET := juke3ds

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM

CC       := $(DEVKITARM)/bin/arm-none-eabi-gcc
TOOLS    := $(DEVKITPRO)/tools/bin
LIBCTRU  := $(DEVKITPRO)/libctru
PORTLIBS := $(DEVKITPRO)/portlibs/3ds

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) \
           -D__3DS__ -I$(LIBCTRU)/include -I$(PORTLIBS)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(LIBCTRU)/lib -L$(PORTLIBS)/lib
LIBS    := -lmpg123 -lctru -lm

all: $(TARGET).3dsx

$(TARGET).elf: source/main.c
> $(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

$(TARGET).smdh:
> $(TOOLS)/smdhtool --create "Juke3DS" "Music player for the 3DS family" "Vexnik" $(LIBCTRU)/default_icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
> $(TOOLS)/3dsxtool $< $@ --smdh=$(TARGET).smdh

clean:
> rm -f $(TARGET).elf $(TARGET).smdh $(TARGET).3dsx
