/*
 * i2cBitBangingBus.cpp
 *
 *  Created on: 06.03.2015
 *      Author: "Marek Wyborski"
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>
#include <complex.h>
#include <unistd.h>
#include <wiringPi.h>
#include <linux/types.h>
#include <stdint.h>
#include <time.h>
#include "i2cbb.h"

// --- I2C bit-level timing, derived from the NXP/Philips I2C-bus
// specification (UM10204), Fast-mode (400 kHz) column - the speed grade
// the si5351 datasheet specifies. i2c_delay() targets the largest of the
// relevant minimums (t_LOW / t_BUF = 1300 ns); that comfortably covers
// the smaller ones (t_HIGH, hold/setup times, all >= 600 ns) used at the
// same call sites below. This replaces a fixed CPU-cycle count that was
// tuned empirically on one board and ran shorter in wall-clock time on
// faster CPUs (e.g. Pi 4 vs Pi Zero 2W) - the likely cause of
// arbitration-lost warnings that only show up on faster hardware.
#define I2C_DELAY_NS 1300L

// The base I2C spec places no upper bound on clock stretching - a slave
// is allowed to hold SCL low indefinitely while it's not ready, so there
// is no spec-derived number to bound the wait with. 35 ms is instead
// borrowed from the SMBus clock-low timeout (T-TIMEOUT): generous enough
// to never trip during a legitimate stretch, short enough to catch a
// genuinely wedged bus in well under a second instead of hanging forever.
#define I2C_CLOCK_STRETCH_TIMEOUT_NS 35000000L

static uint8_t PIN_SDA;
static uint8_t PIN_SCL;
static uint32_t sleepTimeNanos;
static struct timespec nanoSleepTime;
int i2c_started = 0;
static volatile int i2c_bus_error = 0;  // set by arbitration_lost() or a
                                         // clock-stretch timeout; cleared
                                         // at the start of each public
                                         // i2cbb_*() call

void i2cbb_init(uint8_t pin_number_sda, uint8_t pin_number_scl) 
{
	PIN_SDA = pin_number_sda;
	PIN_SCL = pin_number_scl;
	sleepTimeNanos = 0;
	nanoSleepTime.tv_sec = 0;
	nanoSleepTime.tv_nsec = 0;
	i2c_started = 0;
	i2c_bus_error = 0;
  // Pull up setzen 50Kohm
  // http://wiringpi.com/reference/core-functions/
  //    pullUpDnControl(PIN_SDA,PUD_OFF);
  //    pullUpDnControl(PIN_SCL,PUD_OFF);

    nanoSleepTime.tv_sec = 0;
    nanoSleepTime.tv_nsec = 1;

}

// I2C implementation is copied and pasted from wikipedia:
// 
// https://en.wikipedia.org/wiki/I%C2%B2C#Example_of_bit-banging_the_I.C2.B2C_master_protocol
//
//

static int read_SCL(){ // Set SCL as input and return current level of line, 0 or 1
    pinMode(PIN_SCL, INPUT);
    return digitalRead(PIN_SCL);
}

static int read_SDA(){ // Set SDA as input and return current level of line, 0 or 1
    pinMode(PIN_SDA, INPUT);
    return digitalRead(PIN_SDA);
}

static void clear_SCL(){ // Actively drive SCL signal low
    pinMode(PIN_SCL, OUTPUT);
    digitalWrite(PIN_SCL, 0);
}

static void clear_SDA(){ // Actively drive SDA signal low
    pinMode(PIN_SDA, OUTPUT);
    digitalWrite(PIN_SDA, 0);
}

static void arbitration_lost(char * where) {
    printf("I2CBB connection lost:");
    puts(where);
    i2c_bus_error = 1;
}

static void i2c_sleep() {

    if (sleepTimeNanos)
#ifdef NO_NANOSLEEP
        usleep(sleepTimeNanos / 1000);
#else
        nanosleep(&nanoSleepTime, NULL);
#endif
}

static void i2c_delay() {
    // Real-elapsed-time delay (see I2C_DELAY_NS above), not a CPU-cycle
    // count - this keeps the timing correct regardless of core speed.
    struct timespec start, now;
    long elapsed;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (now.tv_sec - start.tv_sec) * 1000000000L
                + (now.tv_nsec - start.tv_nsec);
    } while (elapsed < I2C_DELAY_NS);
}

// Wait for a slave that's stretching the clock to release SCL, bounded by
// I2C_CLOCK_STRETCH_TIMEOUT_NS instead of looping forever. Returns 0 once
// SCL reads high, or -1 (after logging and setting i2c_bus_error) if the
// timeout expires - which used to just hang the program.
static int i2c_wait_scl_high(const char *where) {
    struct timespec start, now;
    long elapsed;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (read_SCL() == 0) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (now.tv_sec - start.tv_sec) * 1000000000L
                + (now.tv_nsec - start.tv_nsec);
        if (elapsed > I2C_CLOCK_STRETCH_TIMEOUT_NS) {
            printf("I2CBB: clock stretch timeout (SCL stuck low) in %s\n", where);
            i2c_bus_error = 1;
            return -1;
        }
        i2c_sleep();
    }
    return 0;
}

static void i2c_start_cond() {
    if (i2c_started) { // if started, do a restart cond
      // set SDA to 1
        read_SDA();
        i2c_delay();
        i2c_wait_scl_high("i2c_start_cond"); // Clock stretching, bounded
        // Repeated start setup time, minimum 4.7us
        i2c_delay();
    }
    if (read_SDA() == 0) {
        arbitration_lost("i2c_start_cond");
    }
    // SCL is high, set SDA from 1 to 0.
    clear_SDA();
    i2c_delay();
    clear_SCL();
    i2c_started = 1;
}

static void i2c_stop_cond(void) {
    // set SDA to 0
    clear_SDA();
    i2c_delay();
    // Clock stretching, bounded - see i2c_wait_scl_high()
    i2c_wait_scl_high("i2c_stop_cond");
    // Stop bit setup time, minimum 4us
    i2c_delay();
//  usleep(4);
    read_SDA();
    // SCL is high, set SDA from 0 to 1
    if (read_SDA() == 0) {
        arbitration_lost("i2c_stop_cond");
    }
    i2c_delay();
    i2c_started = 0;
}

// Write a bit to I2C bus
static void i2c_write_bit(int bit)
{
    if (bit) {
        read_SDA();
    }
    else {
        clear_SDA();
    }
    i2c_delay();

    i2c_wait_scl_high("i2c_write_bit"); // Clock stretching, bounded
    // SCL is high, now data is valid
    // If SDA is high, check that nobody else is driving SDA
    if (bit && read_SDA() == 0) {
        arbitration_lost("i2c_write_bit");
    }
    i2c_delay();
    clear_SCL();
}

// Read a bit from I2C bus
static int i2c_read_bit() {
    int bit;
    // Let the slave drive data
    read_SDA();
    i2c_delay();
    i2c_wait_scl_high("i2c_read_bit"); // Clock stretching, bounded
    // SCL is high, now data is valid
    bit = read_SDA();
    i2c_delay();
    clear_SCL();
	
//  cout << "Bit: " << (bit ? "1" : "0" )<< endl;

    return bit;
}

// Write a byte to I2C bus. Return 0 if ack by the slave.
static int i2c_write_byte(int send_start, int send_stop, uint8_t byte) {
    unsigned bit;
    int nack;
    if (send_start) {
        i2c_start_cond();
    }
    for (bit = 0; bit < 8; bit++) {
        i2c_write_bit((byte & 0x80) != 0);
        byte <<= 1;
    }
    nack = i2c_read_bit();
    if (send_stop) {
        i2c_stop_cond();
    }
    if (i2c_bus_error) {
        // Arbitration loss or a clock-stretch timeout happened somewhere
        // in this byte - treat it as a failed byte even if the ack bit
        // itself happened to read back low.
        nack = 1;
    }
    return nack;
}

// Read a byte from I2C bus
static uint8_t i2c_read_byte(int nack, int send_stop) {
    unsigned char byte = 0;
    unsigned bit;
    for (bit = 0; bit < 8; bit++) {
        byte = (byte << 1) | i2c_read_bit();
    }
    i2c_write_bit(nack);
    if (send_stop) {
        i2c_stop_cond();
    }
    return byte;
}

// KERNEL-LIKE I2C METHODS

// This executes the SMBus "write byte" protocol, returning negative errno else zero on success.
int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    // http://www.totalphase.com/support/articles/200349176-7-bit-8-bit-and-10-bit-I2C-Slave-Addressing
    uint8_t address = (i2c_address << 1) | 0;
    i2c_bus_error = 0;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            if (!i2c_write_byte(0, 1, value) && !i2c_bus_error) {
                return 0;
            }
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    return -1;
}

// This executes the SMBus "read byte" protocol, returning negative errno else a data byte received from the device.
int32_t i2cbb_read_byte_data(uint8_t i2c_address, uint8_t command) {

    uint8_t address = (i2c_address << 1) | 0;
    i2c_bus_error = 0;
    if (!i2c_write_byte(1, 0, address)) {

        if (!i2c_write_byte(0, 0, command)) {

            address = (i2c_address << 1) | 1;
            if (!i2c_write_byte(1, 0, address)) {
                uint8_t value = i2c_read_byte(1, 1);
                if (!i2c_bus_error)
                    return value;
            }
            else
                i2c_stop_cond();
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    return -1;
}

// This executes the SMBus "block write" protocol, returning negative errno else zero on success.
int32_t i2cbb_write_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        const uint8_t * values) {
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    // http://www.totalphase.com/support/articles/200349176-7-bit-8-bit-and-10-bit-I2C-Slave-Addressing
    uint8_t address = (i2c_address << 1) | 0;
    i2c_bus_error = 0;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            int errors = 0;
						size_t i;
            for (i = 0; i < length; i++) {
                if (!errors) {
                    errors = i2c_write_byte(0, 0, values[i]);
                }
            }

            i2c_stop_cond();

            if (!errors && !i2c_bus_error)
                return 0;
						printf("i2cbb: write byte failed at index %d\n", (int)i);
        }
        else{
            i2c_stop_cond();
						printf("i2cbb: command failed\n");
				}
    }
    else{
        i2c_stop_cond();
				printf("i2cbb: address failed\n");
		}
    return -1;
}

// This executes the SMBus "block read" protocol, returning negative errno else the number
// of data bytes in the slave's response.
int32_t i2cbb_read_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        uint8_t* values) {
	uint8_t address = (i2c_address << 1) | 0;
	i2c_bus_error = 0;
/*
	//static int i2c_write_byte(int send_start, int send_stop, uint8_t byte)
	if (i2c_write_byte(1, 0, address)){ 
		i2c_stop_cond();
		printf("i2cbb.c:writing address failed\n");
		return -1;
	}

  if (i2c_write_byte(0, 0, command)){
		i2c_stop_cond();
		printf("i2cbb.c:writing command failed\n");
		return -1;
	}
	i2c_stop_cond();
*/
	static int addr_err_printed = 0;
  address = (i2c_address << 1) | 1;
  if (i2c_write_byte(1, 0, address)) { 
    i2c_stop_cond();
    if (!addr_err_printed) {
      printf("i2cbb.c:writing address failed at %x\n", i2c_address);
      addr_err_printed = 1;
    }
    return -1;
}

	//static uint8_t i2c_read_byte(int nack, int send_stop)
	uint8_t i = 0;
  for (i = 0; i < length - 1; i++) {
  	values[i] = i2c_read_byte(0,0);
    if (i2c_bus_error) {
      i2c_stop_cond();
      printf("i2cbb.c: read failed at byte %d of %d\n", i, length);
      return -1;
    }
  }
	values[i] = i2c_read_byte(1,1);
  if (i2c_bus_error) {
    printf("i2cbb.c: read failed at byte %d of %d\n", i, length);
    return -1;
  }

	i2c_stop_cond();
  return length;
}
