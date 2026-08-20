/* i2cbb.c
 *
 * I2C access for the si5351 (and, eventually, the board-ID
 * EEPROM and INA260 power monitor).
 *
 * This used to bit-bang I2C directly over GPIO from userspace. That
 * approach chased a series of real problems on real hardware - CPU-speed-
 * dependent timing, unbounded clock-stretch waits, and unreliable
 * internal pull-up control on this Pi 4 + wiringPi combination - before
 * i2cdetect/i2cdump confirmed the si5351 is actually wired to the same
 * physical pins (GPIO13/GPIO6) as this board's RTC: a bus the kernel
 * already owns via a live `dtoverlay=i2c-rtc-gpio` driver. Bit-banging
 * those same pins ourselves from userspace would fight that driver for
 * the wire. Instead, this now goes through the kernel's own I2C
 * subsystem (/dev/i2c-N and the standard SMBus ioctls), which correctly
 * implements bus timing for whatever SoC this actually runs on and
 * shares the bus properly with the RTC.
 *
 * The four public read/write functions keep their original signatures
 * and SMBus-style semantics, so si5351v2.c and radio_hw.c did not need
 * to change - only i2cbb_init()'s argument changed, from two GPIO pin
 * numbers to one Linux I2C bus number (the number after "i2c-" in
 * `i2cdetect -l`, e.g. 22 on this board).
 *
 * This intentionally hand-rolls the SMBus ioctl calls instead of linking
 * against libi2c (`-li2c`), which may not be installed - only the kernel
 * UAPI header <linux/i2c-dev.h> is required, which ships alongside a
 * normal toolchain.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include "i2cbb.h"

static int i2c_fd = -1;

void i2cbb_init(int i2c_bus_number)
{
	char path[32];
	snprintf(path, sizeof(path), "/dev/i2c-%d", i2c_bus_number);

	i2c_fd = open(path, O_RDWR);
	if (i2c_fd < 0) {
		printf("i2cbb: failed to open %s: %s - I2C calls will fail\n",
		       path, strerror(errno));
	}
}

/* Minimal SMBus ioctl wrapper - see <linux/i2c-dev.h> for the protocol.
 * addr is set per-call via I2C_SLAVE since each public function here
 * already takes its own i2c_address argument (multiple devices share
 * one open fd/bus, same as the old bit-banged API allowed). */
static int i2c_smbus_xfer(uint8_t addr, uint8_t read_write, uint8_t command,
                           int size, union i2c_smbus_data *data)
{
	if (i2c_fd < 0)
		return -1;

	if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0)
		return -1;

	struct i2c_smbus_ioctl_data args;
	args.read_write = read_write;
	args.command = command;
	args.size = size;
	args.data = data;
	return ioctl(i2c_fd, I2C_SMBUS, &args);
}

// This executes the SMBus "write byte" protocol, returning negative errno else zero on success.
int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value)
{
	union i2c_smbus_data data;
	data.byte = value;

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_WRITE, command,
	                    I2C_SMBUS_BYTE_DATA, &data) < 0) {
		printf("i2cbb: write byte failed (addr 0x%02x, reg 0x%02x): %s\n",
		       i2c_address, command, strerror(errno));
		return -1;
	}
	return 0;
}

// This executes the SMBus "read byte" protocol, returning negative errno else a data byte received from the device.
int32_t i2cbb_read_byte_data(uint8_t i2c_address, uint8_t command)
{
	union i2c_smbus_data data;

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_READ, command,
	                    I2C_SMBUS_BYTE_DATA, &data) < 0) {
		printf("i2cbb: read byte failed (addr 0x%02x, reg 0x%02x): %s\n",
		       i2c_address, command, strerror(errno));
		return -1;
	}
	return data.byte & 0xFF;
}

// This executes the SMBus "block write" protocol, returning negative errno else zero on success.
int32_t i2cbb_write_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        const uint8_t * values)
{
	if (length == 0) {
		// "Point the register pointer, no data" - a plain single-byte
		// write of just the command/register byte, same pattern
		// radio_hw.c uses to kick off a subsequent INA260 read.
		union i2c_smbus_data data; // unused for I2C_SMBUS_BYTE
		if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_WRITE, command,
		                    I2C_SMBUS_BYTE, &data) < 0) {
			printf("i2cbb: address/command failed (addr 0x%02x, reg 0x%02x): %s\n",
			       i2c_address, command, strerror(errno));
			return -1;
		}
		return 0;
	}

	if (length > I2C_SMBUS_BLOCK_MAX) {
		printf("i2cbb: block write length %d exceeds max %d\n",
		       length, I2C_SMBUS_BLOCK_MAX);
		return -1;
	}

	union i2c_smbus_data data;
	data.block[0] = length;
	memcpy(&data.block[1], values, length);

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_WRITE, command,
	                    I2C_SMBUS_I2C_BLOCK_DATA, &data) < 0) {
		printf("i2cbb: block write failed (addr 0x%02x, reg 0x%02x, len %d): %s\n",
		       i2c_address, command, length, strerror(errno));
		return -1;
	}
	return 0;
}

// This executes the SMBus "block read" protocol, returning negative errno else the number
// of data bytes in the slave's response.
int32_t i2cbb_read_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        uint8_t* values)
{
	if (length == 0 || length > I2C_SMBUS_BLOCK_MAX) {
		printf("i2cbb: block read length %d out of range\n", length);
		return -1;
	}

	union i2c_smbus_data data;
	data.block[0] = length;

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_READ, command,
	                    I2C_SMBUS_I2C_BLOCK_DATA, &data) < 0) {
		printf("i2cbb: block read failed (addr 0x%02x, reg 0x%02x, len %d): %s\n",
		       i2c_address, command, length, strerror(errno));
		return -1;
	}

	uint8_t got = data.block[0];
	if (got > length)
		got = length;
	memcpy(values, &data.block[1], got);
	return got;
}
