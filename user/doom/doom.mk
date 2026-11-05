# Doom port makefile for FortressOS

DOOM_DIR := user/doom
DOOM_BUILD := $(BUILD_DIR)/doom
DOOM_UPSTREAM := $(DOOM_DIR)/upstream

DOOM_SRC_CORE := dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c \
                 d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c \
                 hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c \
                 i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c \
                 m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c \
                 p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c \
                 p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c \
                 p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c \
                 r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c \
                 st_lib.c st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c \
                 w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c

DOOM_UPSTREAM_OBJS := $(addprefix $(DOOM_BUILD)/upstream_,$(patsubst %.c,%.o,$(DOOM_SRC_CORE)))
DOOM_RUNTIME_OBJS := $(DOOM_BUILD)/doom_runtime.o $(DOOM_BUILD)/doomgeneric_fortress.o $(DOOM_BUILD)/doom_start.o

DOOM_FREESTANDING_FLAGS := -ffreestanding -fno-builtin -nostdlib -Os -mno-red-zone \
                           -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables \
                           -Wall -Wno-unused-result -Wno-stringop-truncation -Wno-format-overflow \
                           -Wno-pointer-to-int-cast -Wno-dangling-pointer \
                           -I$(DOOM_DIR)/include -I$(DOOM_UPSTREAM) -I. \
                           -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE

$(DOOM_BUILD):
	@mkdir -p $(DOOM_BUILD)

$(DOOM_BUILD)/upstream_%.o: $(DOOM_UPSTREAM)/%.c | $(DOOM_BUILD)
	@$(CC) $(DOOM_FREESTANDING_FLAGS) -c $< -o $@

$(DOOM_BUILD)/doom_runtime.o: $(DOOM_DIR)/doom_runtime.c $(DOOM_DIR)/doom_runtime.h | $(DOOM_BUILD)
	@$(CC) $(DOOM_FREESTANDING_FLAGS) -c $< -o $@

$(DOOM_BUILD)/doomgeneric_fortress.o: $(DOOM_DIR)/doomgeneric_fortress.c $(DOOM_DIR)/doom_runtime.h | $(DOOM_BUILD)
	@$(CC) $(DOOM_FREESTANDING_FLAGS) -c $< -o $@

$(DOOM_BUILD)/doom_start.o: $(DOOM_DIR)/start.asm | $(DOOM_BUILD)
	@$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/doom.elf: $(DOOM_BUILD)/doom_start.o $(DOOM_RUNTIME_OBJS) $(DOOM_UPSTREAM_OBJS) user/shell.ld
	@echo "  LD      $@"
	@$(LD) -m elf_x86_64 -nostdlib -static -z noexecstack --gc-sections -T user/shell.ld \
	       $(DOOM_BUILD)/doom_start.o $(DOOM_BUILD)/doom_runtime.o $(DOOM_BUILD)/doomgeneric_fortress.o \
	       $(DOOM_UPSTREAM_OBJS) -o $@

DOOM_HOST_SAN ?= $(shell printf 'int main(void){return 0;}\n' | $(CC) -x c - -fsanitize=address,undefined -o /dev/null 2>/dev/null && echo "-fsanitize=address,undefined")

# Host test harness
HOST_DOOM_BIN := $(BUILD_DIR)/test_doom_host

$(HOST_DOOM_BIN): tests/doom_host.c $(DOOM_DIR)/doom_runtime.c $(DOOM_DIR)/doomgeneric_fortress.c $(addprefix $(DOOM_UPSTREAM)/,$(DOOM_SRC_CORE)) | $(DOOM_BUILD)
	@echo "  HOST_CC $@"
	@$(CC) $(DOOM_HOST_SAN) -g -O2 -Wall \
	       -Wno-unused-result -Wno-stringop-truncation -Wno-format-overflow \
	       -Wno-pointer-to-int-cast -Wno-dangling-pointer \
	       -DDOOM_HOST_TEST -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE \
	       -I. -I$(DOOM_UPSTREAM) \
	       tests/doom_host.c $(DOOM_DIR)/doom_runtime.c $(DOOM_DIR)/doomgeneric_fortress.c \
	       $(addprefix $(DOOM_UPSTREAM)/,$(DOOM_SRC_CORE)) \
	       -lm -o $@

.PHONY: test-doom-host test-doom-qemu
test-doom-host: $(HOST_DOOM_BIN)
	@$(HOST_DOOM_BIN)

test-doom-qemu: $(BUILD_DIR)/doom.elf $(BOOTABLE_IMG)
	@python3 scripts/test_doom_qemu.py
