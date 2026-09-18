.PHONY: all help test test-userspace install clean

CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
WARNFLAGS = -std=gnu11 -Wall -Wextra -Werror
HARDEN_CFLAGS = -fstack-protector-strong -D_FORTIFY_SOURCE=2
HARDEN_LDFLAGS = -Wl,-z,relro,-z,now
LDLIBS_DYNBLK = -ldl
NATIVE_SOURCES = dynblk_cli.c dynblk_common.c dynblk_check.c dynblk_engine.c dynblk_host.c lzo/decompress.c
NATIVE_HEADERS = dynblk_engine.h dynblk_host.h dynblk_mount.inc dynblk_common.h dynblk_check.h dynblk_uapi.h lzo/decompress.h lzo/compat.h

all: dynblk mount.dynblk

mount.dynblk: dynblk
	ln -sfn dynblk "$@"

help:
	@printf '%s\n' 'make dynblk' 'make dynblk-initrd' 'make test' \
		'make install [DESTDIR=/staging/root]' \
		'Kernel: make -C /path/to/headers M="$$PWD" modules'

dynblk: $(NATIVE_SOURCES) $(NATIVE_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) $(HARDEN_CFLAGS) \
		$(NATIVE_SOURCES) -o "$@" $(LDFLAGS) $(HARDEN_LDFLAGS) $(LDLIBS_DYNBLK)

dynblk-initrd: $(NATIVE_SOURCES) $(NATIVE_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) $(HARDEN_CFLAGS) \
		-DDYNBLK_NO_DYNAMIC_CODECS -static -s $(NATIVE_SOURCES) -o "$@" $(LDFLAGS)

lzo/test-codec.so: lzo/compress-test.c lzo/decompress.c lzo/compat.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -fPIC -shared \
		lzo/compress-test.c lzo/decompress.c -o "$@" $(LDFLAGS)
lzo/test-sanitize: lzo/test.c lzo/compress-test.c lzo/decompress.c lzo/compat.h
	$(CC) $(CPPFLAGS) -std=gnu11 -O1 -g -Wall -Wextra -Werror \
		-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie \
		lzo/test.c lzo/compress-test.c lzo/decompress.c -o "$@" $(LDFLAGS)

test-userspace: dynblk mount.dynblk dynblk-initrd lzo/test-codec.so lzo/test-sanitize
	./lzo/test-sanitize
	PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -m unittest -v tests.test_native tests.test_mount

test: test-userspace
	$(MAKE) -C tests-storage test $(if $(BUILD_DIR),BUILD_DIR="$(BUILD_DIR)")

install: dynblk mount.dynblk
	install -d "$(DESTDIR)/usr/sbin" "$(DESTDIR)/usr/share/doc/dynblk/lzo"
	install -m 0755 dynblk "$(DESTDIR)/usr/sbin/dynblk"
	ln -sfn dynblk "$(DESTDIR)/usr/sbin/mount.dynblk"
	install -m 0644 README.md FORMAT.md LICENSE "$(DESTDIR)/usr/share/doc/dynblk/"
	install -m 0644 lzo/README.md lzo/decompress.c lzo/decompress.h lzo/compat.h \
		"$(DESTDIR)/usr/share/doc/dynblk/lzo/"

clean:
	rm -f dynblk mount.dynblk dynblk-initrd lzo/test-codec.so lzo/test-sanitize
	rm -f *.o .*.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order
	$(MAKE) -C tests-storage clean
