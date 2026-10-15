# Build against the installed GATTLIB (sudo make install in gattlib-src/build)
CFLAGS  += -Wall -Wextra -O2 $(shell pkg-config --cflags gattlib gio-2.0)
LDLIBS  += $(shell pkg-config --libs gattlib gio-2.0) -lpthread

my_write: my_write.c

clean:
	rm -f my_write

.PHONY: clean
