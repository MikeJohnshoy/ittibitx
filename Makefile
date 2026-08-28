CC      := gcc
# -march=native detects this machine's actual CPU (NEON on the Pi) at
# build time - minibitx should be built directly on the Pi
# it'll run on, but a binary built this way
# shouldn't be copied to a different Pi model; drop -march=native if
# that's ever needed. It's what lets antialias.c's branch-free FIR loop
# (see antialias.c) actually vectorize instead of just being eligible to.
CFLAGS  := -O3 -march=native -Wall -Wextra -std=gnu11
LDFLAGS := -lm -lasound -lpthread -ldl -lwiringPi
SRC := src/minibitx.c src/radio.c src/radio_hw.c src/hpsdr_p1.c src/usb_gadget.c src/status.c src/i2c.c src/si5351v2.c src/sound.c src/vfo.c src/hamlib.c src/hw_settings.c src/antialias.c src/cw.c
OBJ := $(SRC:.c=.o)

all: minibitx

minibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) minibitx

