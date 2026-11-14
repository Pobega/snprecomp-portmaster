ADB ?= $(shell command -v adb 2>/dev/null || echo $(HOME)/.local/bin/adb)
JOBS ?= $(shell nproc)

# NextUI (PORTS.pak) SD card layout (TrimUI Brick / Smart Pro, RGSP / H700, etc.)
NEXTUI_PORTS_DIR ?= /mnt/SDCARD/Roms/Ports (PORTS)
NEXTUI_DOTPORTS_DIR ?= $(NEXTUI_PORTS_DIR)/.ports
NEXTUI_GAMEDIR ?= $(NEXTUI_DOTPORTS_DIR)/sinandpunishment

STAGE_DIR := dist/portmaster-pr/sinandpunishment
TOOLBOX_RUN := $(if $(wildcard /run/.containerenv),,toolbox run --container fedora-toolbox-44)

.PHONY: all build host test package push push-fast logs

all: build

build:
	bash tools/build-portmaster-aarch64.sh

host:
	@if [ ! -f build-host/build.ninja ]; then \
		$(TOOLBOX_RUN) cmake -B build-host -G Ninja -DSNP_BACKEND_GLES3=ON -DCMAKE_BUILD_TYPE=Release; \
	fi
	$(TOOLBOX_RUN) ninja -C build-host -j$(JOBS)

test:
	@if [ ! -f build-gles3-test/build.ninja ]; then \
		$(TOOLBOX_RUN) cmake -B build-gles3-test -G Ninja -DSNP_BACKEND_GLES3=ON -DSNP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release; \
	fi
	$(TOOLBOX_RUN) ninja -C build-gles3-test test_gles3_renderer
	./build-gles3-test/test_gles3_renderer

package:
	@if [ -f build-portmaster-aarch64/SnPRecompiled ]; then \
		PACKAGE_ONLY=1 bash tools/build-portmaster-aarch64.sh; \
	else \
		bash tools/build-portmaster-aarch64.sh; \
	fi

push: build
	@$(ADB) get-state >/dev/null
	@echo "==> Pushing Sin and Punishment.sh to Ports directories..."
	$(ADB) push "$(STAGE_DIR)/Sin and Punishment.sh" "$(NEXTUI_PORTS_DIR)/Sin and Punishment.sh"
	$(ADB) push "$(STAGE_DIR)/Sin and Punishment.sh" "$(NEXTUI_DOTPORTS_DIR)/Sin and Punishment.sh"
	@echo "==> Preparing $(NEXTUI_GAMEDIR)/ directories..."
	$(ADB) shell 'mkdir -p "$(NEXTUI_GAMEDIR)/licenses" "$(NEXTUI_GAMEDIR)/roms" "$(NEXTUI_GAMEDIR)/saves"'
	@echo "==> Pushing release files to $(NEXTUI_GAMEDIR)/..."
	$(ADB) push \
		"$(STAGE_DIR)/README.md" \
		"$(STAGE_DIR)/gameinfo.xml" \
		"$(STAGE_DIR)/port.json" \
		"$(STAGE_DIR)/screenshot.png" \
		"$(STAGE_DIR)/sinandpunishment/snprecomp.toml" \
		"$(STAGE_DIR)/sinandpunishment/snprecompiled.aarch64" \
		"$(NEXTUI_GAMEDIR)/"
	$(ADB) push $(STAGE_DIR)/sinandpunishment/licenses/* "$(NEXTUI_GAMEDIR)/licenses/"
	@echo "==> Cleaning stale binary..."
	$(ADB) shell 'rm -f "$(NEXTUI_GAMEDIR)/SnPRecompiled"; sync'
	@echo "==> Done!"

push-fast:
	@$(ADB) get-state >/dev/null
	@echo "==> Pushing Sin and Punishment.sh to Ports directories..."
	$(ADB) push "portmaster/Sin and Punishment.sh" "$(NEXTUI_PORTS_DIR)/Sin and Punishment.sh"
	$(ADB) push "portmaster/Sin and Punishment.sh" "$(NEXTUI_DOTPORTS_DIR)/Sin and Punishment.sh"
	@echo "==> Pushing snprecompiled.aarch64 and config directly to $(NEXTUI_GAMEDIR)/..."
	$(ADB) push "portmaster/sinandpunishment/snprecompiled.aarch64" "portmaster/sinandpunishment/snprecomp.toml" "$(NEXTUI_GAMEDIR)/"
	$(ADB) shell 'rm -f "$(NEXTUI_GAMEDIR)/SnPRecompiled"; sync'
	@echo "==> Done!"

logs:
	@$(ADB) shell 'cat "$(NEXTUI_GAMEDIR)/log.txt" 2>/dev/null'
