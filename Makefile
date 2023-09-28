
# CFLAGS="-DDEBUG"
CFLAGS=

all:
	clang ${CFLAGS} -arch arm64 -fPIC -shared -Wall -Werror -o mylib.so mylib.c libc.so.6

clean:
	rm mylib.so