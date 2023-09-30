
# CFLAGS="-DDEBUG"
CFLAGS=

all:
	clang ${CFLAGS} --target=aarch64-linux-gnu -fPIC -shared -Wall -Werror -o vacuumstreamer.so vacuumstreamer.c libc.so.6_r2228

clean:
	rm mylib.so
