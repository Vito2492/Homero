# Mansion — build
#   make            Linux / macOS build (needs SDL2 development files)
#   make windows    Windows build from Linux with MinGW-w64 (set SDL2_MINGW to the
#                   x86_64-w64-mingw32 folder of SDL2's "devel-mingw" package)
#   make test       runs every screen without a display and saves pictures in test-out/
#                   (needs the ROM and snes9x_libretro.so next to the program)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-misleading-indentation
SDL2_MINGW ?= SDL2-2.30.9/x86_64-w64-mingw32

all: mansion

mansion: src/mansion.c src/libretro.h
	$(CC) $(CFLAGS) -o $@ src/mansion.c $$(sdl2-config --cflags --libs)

windows: Mansion.exe

Mansion.exe: src/mansion.c src/libretro.h
	x86_64-w64-mingw32-gcc $(CFLAGS) -o $@ src/mansion.c -I$(SDL2_MINGW)/include/SDL2 -Dmain=SDL_main \
		-L$(SDL2_MINGW)/lib -lmingw32 -lSDL2main -lSDL2 -mwindows -static-libgcc

test: mansion
	mkdir -p test-out
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./mansion --test test-out

clean:
	rm -f mansion Mansion.exe
	rm -rf test-out

.PHONY: all windows test clean
