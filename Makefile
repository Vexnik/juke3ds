.RECIPEPREFIX := >

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

all: musicplayer.3dsx

musicplayer.elf: source/main.c
> $(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

musicplayer.smdh:
> $(TOOLS)/smdhtool --create "Music Player" "MP3 and WAV player" "Homebrew" $(LIBCTRU)/default_icon.png $@

musicplayer.3dsx: musicplayer.elf musicplayer.smdh
> $(TOOLS)/3dsxtool $< $@ --smdh=musicplayer.smdh

clean:
> rm -f musicplayer.elf musicplayer.smdh musicplayer.3dsx

