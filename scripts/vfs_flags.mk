.PHONY: test-vfs-flags-host
test-vfs-flags-host:
	@mkdir -p build
	@gcc -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -no-pie -DFORTRESS_DAC_ENFORCED -ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/pipe_host -Itests/host -Isrc/include -Isrc/fs -Isrc/drivers -Isrc/mm -Isrc/kernel -Isrc/arch/x86_64 tests/vfs_open_flags_host.c src/kernel/creds.c src/fs/permission_values.c -o build/vfs_open_flags_host
	@build/vfs_open_flags_host
