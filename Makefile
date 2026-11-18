# Dev conveniences; the port itself builds from reconstruct/.
STX := Garfield - Big, Fat, Hairy Deal.stx

# Extracts the game's files from the disk image for the port's assets.
assets: build/emulator/extract
	./build/emulator/extract "$(STX)" extracted

build/emulator/extract: emulator/extract.c emulator/disk.c emulator/stx.c emulator/disk.h emulator/stx.h
	mkdir -p build/emulator
	$(CC) -std=gnu11 -O2 -Wall -Wextra -o $@ emulator/extract.c emulator/disk.c emulator/stx.c

# Reference emulator vs. hybrid, driven by emulator/lockstep.lua.
lockstep:
	$(MAKE) -C reconstruct dev
	./reconstruct/build/garfield-dev emulator/lockstep.lua $(FRAMES)

# The port against the reference at every sync point: scripted walk (portlock) or the autoplayer visiting rooms
# (autolock, with savestates of both sides); BUDGET frames of plans, RANDOM_PLANS extra random plans a room.
portlock:
	$(MAKE) -C reconstruct dev
	./reconstruct/build/garfield-dev emulator/portlock.lua $(FRAMES)

autolock:
	$(MAKE) -C reconstruct dev
	./reconstruct/build/garfield-dev emulator/autolock.lua $(or $(BUDGET),30000) "" $(or $(RANDOM_PLANS),0)

# Every room through a door, port against reference (what the autoplayer cannot walk to is reached this way).
teleport:
	$(MAKE) -C reconstruct dev
	for n in $$(seq 1 80); do ./reconstruct/build/garfield-dev emulator/teleport.lua $$n 300 || exit 1; done

.PHONY: assets lockstep portlock autolock teleport
