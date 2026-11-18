CC      ?= gcc
PKG     := $(if $(IN_NIX_SHELL),pkg-config,PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig pkg-config)
CFLAGS  := -m32 -std=c11 -O2 -Wall -Wextra $(shell $(PKG) --cflags sdl3)
LDFLAGS := -m32
LDLIBS  := $(shell $(PKG) --libs sdl3)

# The port and the dev build share every source except the one that holds main(): main.c or devmain.c.
CORE := src/game.c src/unknown.c src/sdl.c
SRC := $(CORE) src/main.c
OBJ := $(SRC:src/%.c=build/%.o)

build/garfield: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS) -c -o $@ $<

# Dev build: the port's game code plus Lua scripting and the reference emulator (never part of the port).
LUA_DIR  := ../third_party/lua/src
LUA_SRC  := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c,$(wildcard $(LUA_DIR)/*.c))
LUA_OBJ  := $(LUA_SRC:$(LUA_DIR)/%.c=build/lua/%.o)
DEV_FLAGS := -m32 -std=gnu11 -O2 -Wall -Wextra -Isrc -I../emulator -I$(LUA_DIR) $(shell $(PKG) --cflags unicorn sdl3)

dev: build/garfield-dev

build/garfield-dev: src/devmain.c build/game.o build/unknown.o build/sdl.o ../emulator/emu.c ../emulator/hybrid.c ../emulator/guestmem.c ../emulator/disk.c ../emulator/stx.c $(LUA_OBJ) \
		$(wildcard src/*.h) ../emulator/emu.h ../emulator/hybrid.h ../emulator/disk.h ../emulator/stx.h
	$(CC) $(DEV_FLAGS) -o $@ src/devmain.c build/game.o build/unknown.o build/sdl.o ../emulator/emu.c ../emulator/hybrid.c ../emulator/guestmem.c ../emulator/disk.c ../emulator/stx.c $(LUA_OBJ) \
		$(shell $(PKG) --libs unicorn sdl3) -lm -ldl

build/lua/%.o: $(LUA_DIR)/%.c | build/lua
	$(CC) -m32 -O2 -DLUA_USE_POSIX -w -c -o $@ $<

build build/lua:
	mkdir -p $@

run: build/garfield
	./build/garfield ../extracted

clean:
	rm -rf build

.PHONY: run clean dev

# Not made from anything (make would otherwise try Makefile.web, a CWEB file to it).
Makefile Makefile.web: ;
