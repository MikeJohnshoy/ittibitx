CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
LDFLAGS := -lm -lasound -lpthread -ldl -lwiringPi
SRC := src/minibitx.c src/radio.c src/radio_hw.c src/hpsdr_p1.c src/usb_gadget.c src/status.c src/i2c.c src/si5351v2.c src/sound.c src/vfo.c src/hamlib.c src/hw_settings.c
OBJ := $(SRC:.c=.o)

all: minibitx

minibitx: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) minibitx

