# SPDX-License-Identifier: GPL-3.0-or-later
# pokemmo-ps5. Run these targets inside the build environment (pokemmo-ps5-buildenv):
#   ../pokemmo-ps5-buildenv/ps5env make <target>
SHELL := bash

# Homebrew range: retail ids are far lower; PS5 homebrew uses PPSA99xxx (homebrew.page catalog), 98xxx was free.
LOADER_TITLE_ID ?= PPSA98001
# Optional: your PC's LAN address; the loader also sends its UDP log there (it always broadcasts too).
# PROBE_LOG_HOST, its old name, still works.
LOG_HOST ?= $(PROBE_LOG_HOST)
CLIENT_ZIP ?= private/PokeMMO-Client.zip
# The game files shipped inside the title (assets/roms, mounted as the client's /game/roms). Not in git.
ROMS_DIR ?= private/roms

.PHONY: help env-check loader host-loader host-run image-loader clean

help:
	@echo "make loader         build the title (folder install, dist/pokemmo-ps5-loader-$(LOADER_TITLE_ID).zip; LOG_HOST=192.168.x.y optional)"
	@echo "make image-loader   the loader as a .ffpfsc image (dist/)"
	@echo "make host-loader    build the same loader for this Linux PC (build/host/pokemmo-host); host-run runs the client in it"
	@echo "make clean          remove build outputs"

env-check:
	@[[ -n "$$PS5_NATIVE_APP_TEMPLATE" && -n "$$PS5_OPENGL_PREFIX" ]] || { \
		echo "Not inside the build environment. Use: ../pokemmo-ps5-buildenv/ps5env make $(MAKECMDGOALS)"; exit 2; }

loader: env-check
	@rm -rf build/loader-assets && mkdir -p build/loader-assets/roms
	@if [[ -n "$(LOG_HOST)" ]]; then echo "$(LOG_HOST)" > build/loader-assets/loghost.txt; \
	 else echo "(no LOG_HOST: UDP broadcast only)" > build/loader-assets/README.txt; fi
	@roms=$$(find "$(ROMS_DIR)" -maxdepth 1 -type f \( -iname '*.nds' -o -iname '*.gba' \) 2>/dev/null | wc -l); \
	 if [[ $$roms -eq 0 ]]; then \
		echo "No ROMs in $(ROMS_DIR): put the .nds/.gba game files there (the folder is git-ignored) and build again."; exit 2; \
	 fi; \
	 find "$(ROMS_DIR)" -maxdepth 1 -type f \( -iname '*.nds' -o -iname '*.gba' \) -exec cp {} build/loader-assets/roms/ \; ; \
	 echo "Preloaded $$roms ROM(s) from $(ROMS_DIR) into assets/roms"
	bash scripts/runtime-libs.sh build/loader-assets/lib
	cp assets/settings/defaults.properties build/loader-assets/defaults.properties
	bash scripts/build-title.sh --title-id $(LOADER_TITLE_ID) --name "PokeMMO" \
		--sources loader/src --sources loader/platform/ps5 --include loader/include --zlib \
		--assets build/loader-assets --content-suffix LOADER --download-mib 2048 \
		--branding assets/branding
	@mkdir -p dist && cp build/titles/$(LOADER_TITLE_ID)/dist/$(LOADER_TITLE_ID).zip dist/pokemmo-ps5-loader-$(LOADER_TITLE_ID).zip

# The same title as one compressed image (.ffpfsc) for ShadowMountPlus, built by the boilerplate's packaging (MkPFS).
image-loader: loader
	$(MAKE) -C build/titles/$(LOADER_TITLE_ID) --no-print-directory ffpfsc
	@mkdir -p dist && cp build/titles/$(LOADER_TITLE_ID)/dist/$(LOADER_TITLE_ID).ffpfsc dist/$(LOADER_TITLE_ID).ffpfsc
	@echo "Image: dist/$(LOADER_TITLE_ID).ffpfsc"

# The loader on this PC (no console needed): same adapters, platform/host. Needs a C compiler and zlib headers.
host-loader:
	$(MAKE) -f loader/Makefile.host --no-print-directory

host-run: host-loader
	@[[ -f "$(CLIENT_ZIP)" ]] || { echo "no client at $(CLIENT_ZIP): put the PokeMMO client zip there first"; exit 2; }
	rm -rf build/host/client build/host/root && mkdir -p build/host/client
	unzip -q "$(CLIENT_ZIP)" -d build/host/client -x 'bin/win*' 'bin/mac*' 'bin/linux/arm64/*' '*.exe'
	./build/host/pokemmo-host --client build/host/client --root build/host/root --timeout 60

clean:
	rm -rf build dist
