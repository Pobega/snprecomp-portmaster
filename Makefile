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
		$(TOOLBOX_RUN) cmake -S tests -B build-gles3-test -G Ninja -DSNP_BACKEND_GLES3=ON -DSNP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release; \
	fi
	$(TOOLBOX_RUN) ninja -C build-gles3-test test_gles3_renderer test_snp_patch test_ra_client
	./build-gles3-test/test_gles3_renderer
	./build-gles3-test/test_snp_patch
	./build-gles3-test/test_ra_client

package:
	@if [ -f build-portmaster-aarch64/SnPRecompiled ] && [ -f build-portmaster-aarch64/snp-patch ]; then \
		PACKAGE_ONLY=1 bash tools/build-portmaster-aarch64.sh; \
	else \
		bash tools/build-portmaster-aarch64.sh; \
	fi

push: build
	@if [ ! -f "$(STAGE_DIR)/sinandpunishment/snp.patch" ]; then \
		PACKAGE_ONLY=1 bash tools/build-portmaster-aarch64.sh; \
	fi
	@$(ADB) get-state >/dev/null
	@echo "==> Pushing Sin and Punishment.sh to Ports directories..."
	$(ADB) push "$(STAGE_DIR)/Sin and Punishment.sh" "$(NEXTUI_PORTS_DIR)/Sin and Punishment.sh"
	$(ADB) push "$(STAGE_DIR)/Sin and Punishment.sh" "$(NEXTUI_DOTPORTS_DIR)/Sin and Punishment.sh"
	@echo "==> Preparing $(NEXTUI_GAMEDIR)/ directories..."
	$(ADB) shell 'mkdir -p "$(NEXTUI_GAMEDIR)/tools" "$(NEXTUI_GAMEDIR)/licenses" "$(NEXTUI_GAMEDIR)/roms" "$(NEXTUI_GAMEDIR)/saves"'
	@echo "==> Pushing release files to $(NEXTUI_GAMEDIR)/..."
	$(ADB) push \
		"$(STAGE_DIR)/README.md" \
		"$(STAGE_DIR)/gameinfo.xml" \
		"$(STAGE_DIR)/port.json" \
		"$(STAGE_DIR)/screenshot.png" \
		"$(STAGE_DIR)/sinandpunishment/snp.patch" \
		"$(NEXTUI_GAMEDIR)/"
	$(ADB) push "$(STAGE_DIR)/sinandpunishment/tools/patchscript" "$(STAGE_DIR)/sinandpunishment/tools/snp-patch" "$(NEXTUI_GAMEDIR)/tools/"
	$(ADB) push $(STAGE_DIR)/sinandpunishment/licenses/* "$(NEXTUI_GAMEDIR)/licenses/"
	@$(ADB) shell '[ -f "$(NEXTUI_GAMEDIR)/snprecomp.toml" ]' >/dev/null 2>&1 || $(ADB) push "$(STAGE_DIR)/sinandpunishment/snprecomp.toml" "$(NEXTUI_GAMEDIR)/"
	@echo "==> Cleaning previous binaries and stored ROMs so PortMaster patcher runs on boot..."
	$(ADB) shell 'rm -f "$(NEXTUI_GAMEDIR)/snprecompiled.aarch64" "$(NEXTUI_GAMEDIR)/SnPRecompiled" "$(NEXTUI_GAMEDIR)/snp.n64.en.fan.z64" "$(NEXTUI_GAMEDIR)/snp.n64.jp.1.0.z64"; sync'
	@echo "==> Done!"

push-fast:
	@$(ADB) get-state >/dev/null
	@echo "==> Pushing Sin and Punishment.sh to Ports directories..."
	$(ADB) push "portmaster/Sin and Punishment.sh" "$(NEXTUI_PORTS_DIR)/Sin and Punishment.sh"
	$(ADB) push "portmaster/Sin and Punishment.sh" "$(NEXTUI_DOTPORTS_DIR)/Sin and Punishment.sh"
	@echo "==> Pushing unencrypted snprecompiled.aarch64 and tools directly to $(NEXTUI_GAMEDIR)/..."
	$(ADB) shell 'mkdir -p "$(NEXTUI_GAMEDIR)/tools"'
	$(ADB) push "build-portmaster-aarch64/SnPRecompiled" "$(NEXTUI_GAMEDIR)/snprecompiled.aarch64"
	$(ADB) push "portmaster/sinandpunishment/tools/patchscript" "build-portmaster-aarch64/snp-patch" "$(NEXTUI_GAMEDIR)/tools/"
	@$(ADB) shell '[ -f "$(NEXTUI_GAMEDIR)/snprecomp.toml" ]' >/dev/null 2>&1 || $(ADB) push "portmaster/sinandpunishment/snprecomp.toml" "$(NEXTUI_GAMEDIR)/"
	$(ADB) shell 'rm -f "$(NEXTUI_GAMEDIR)/SnPRecompiled" "$(NEXTUI_GAMEDIR)/snp.patch"; sync'
	@echo "==> Done!"

logs:
	@$(ADB) shell 'cat "$(NEXTUI_GAMEDIR)/patchlog.txt" 2>/dev/null; echo "---"; cat "$(NEXTUI_GAMEDIR)/log.txt" 2>/dev/null'
