# Atari 2600 Video Olympics netplay relay -- C implementation.
#
# The Python server (../vo_relay_server.py) stays canonical; this binary is
# held to it by tools/server_diff.py.  See README.md.

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wvla -Wpointer-arith
CPPFLAGS += -D_GNU_SOURCE
LDLIBS   := -lpthread

TARGET := vo-relay
SRCS   := src/vo-relay.c src/lobby.c
HDRS   := src/lobby.h

.PHONY: all strict asan clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(LDLIBS)

# Gate 0: no warnings, at all.
strict: CFLAGS += -Werror
strict: clean $(TARGET)

# Gate 0: run the differential under ASan/UBSan.  There is no exception net in
# C -- a latent bug that Python would log and shrug off takes the relay down.
asan: CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Wshadow \
                -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean $(TARGET)

clean:
	$(RM) $(TARGET)
