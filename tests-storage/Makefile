# Test the shared production engine, not a separately reimplemented format.
BUILD_DIR ?= .build
CC = gcc
# Keep required test encoders/sanitizers even when packaging supplies CFLAGS.
TEST_CFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -Werror -DDYNBLK_ENGINE_TEST \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
SOURCES = engine-tool.c ../dynblk_engine.c ../dynblk_host.c ../dynblk_common.c ../lzo/decompress.c
.PHONY: test clean
test: $(BUILD_DIR)/engine-tool $(BUILD_DIR)/fault
	$(BUILD_DIR)/fault
	cd .. && PYTHONDONTWRITEBYTECODE=1 DYNBLK_ENGINE_TOOL="$(abspath $(BUILD_DIR)/engine-tool)" \
		python3 -m unittest -v tests.test_engine tests.test_reclaim
$(BUILD_DIR)/engine-tool: $(SOURCES) ../dynblk_engine.h ../dynblk_host.h ../dynblk_common.h
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(SOURCES) $(LDFLAGS) -ldl -o "$@"
$(BUILD_DIR)/fault: fault.c ../dynblk_engine.c ../dynblk_engine.h
	mkdir -p "$(BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) fault.c ../dynblk_engine.c $(LDFLAGS) -l:liblz4.so.1 -o "$@"
clean:
	rm -rf "$(BUILD_DIR)"
