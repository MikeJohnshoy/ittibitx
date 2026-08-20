/* i2c.h
 *
 * Kernel-backed I2C access (via /dev/i2c-N and the standard SMBus ioctls)
 * for the si5351 clock generator and the INA260 power monitor. Originally
 * a GPIO bit-banging implementation ("i2cBitBangingBus.h" / "i2cbb.h",
 * based on Mark Wyborski's work, re-written for C by Ashhar Farhan,
 * VU2ESE) - see i2c.c for why it was replaced and renamed.
 */

// Opens the given Linux I2C bus (the number after "i2c-" as shown by
// `i2cdetect -l`, e.g. 22) via the kernel's i2c-dev interface. Replaces
// the old two-GPIO-pin-number bit-banging init - see i2c.c for why.
void i2c_init(int i2c_bus_number);

// This executes the SMBus "write byte" protocol, returning negative errno else zero on success.
int32_t i2c_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value);

// This executes the SMBus "read byte" protocol, returning negative errno
// else a data byte received from the device.
int32_t i2c_read_byte_data(uint8_t i2c_address, uint8_t command);

// This executes the SMBus "block write" protocol, returning negative errno else zero on success.
int32_t i2c_write_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length,
	const uint8_t * values);

// This executes the SMBus "block read" protocol, returning negative errno else the number
// of data bytes in the slave's response.
int32_t i2c_read_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length, uint8_t* values);

