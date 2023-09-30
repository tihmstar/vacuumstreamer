
# CFLAGS="-DDEBUG"
CFLAGS=

all:
	clang ${CFLAGS} --target=aarch64-linux-gnu -fPIC -shared -Wall -Werror -o mylib.so mylib.c libc.so.6

clean:
	rm mylib.so