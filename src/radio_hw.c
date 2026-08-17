/*
 * radio_hw.c — see radio_hw.h for scope. Ported from sbitx's radio_hw.c.
 */

#include <stdio.h>
#include <stdint.h>
#include <wiringPi.h>
#include "i2cbb.h"
#include "radio_hw.h"

#define DEBUG 0

/* ---- INA260 power monitor (I2C address 0x40) -------------------------- */

#define INA260_ADDRESS   0x40
#define CONFIG_REGISTER  0x00
#define VOLTAGE_REGISTER 0x02
#define CURRENT_REGISTER 0x01
#define CONFIG_DEFAULT   0x6127 // Continuous mode, default averaging

/* ---- Boot-time GPIO setup ---------------------------------------------- */

int radio_hw_gpio_init(void)
{
	if (wiringPiSetupGpio() < 0)
		return -1;

	pinMode(TX_LINE, OUTPUT);
	pinMode(TX_POWER, OUTPUT);
	pinMode(EXT_PTT, OUTPUT);
	pinMode(LPF_A, OUTPUT);
	pinMode(LPF_B, OUTPUT);
	pinMode(LPF_C, OUTPUT);
	pinMode(LPF_D, OUTPUT);

	digitalWrite(LPF_A, LOW);
	digitalWrite(LPF_B, LOW);
	digitalWrite(LPF_C, LOW);
	digitalWrite(LPF_D, LOW);
	digitalWrite(EXT_PTT, LOW);
	digitalWrite(TX_LINE, LOW);
	digitalWrite(TX_POWER, LOW);

	return 0;
}

/* ---- Board hardware revision -------------------------------------------- */

int radio_hw_detect_version(void)
{
	uint8_t response[4];
	if (i2cbb_read_i2c_block_data(0x8, 0, 4, response) == -1)
		return SBITX_DE;
	else
		return SBITX_V2;
}

/* ---- Low-pass filter band switching -------------------------------------- */

static int prev_lpf = -1;
void set_lpf_40mhz(int frequency)
{
	int lpf = 0;

	if (frequency < 5500000)
		lpf = LPF_D;
	else if (frequency < 10500000)
		lpf = LPF_C;
	else if (frequency < 18500000)
		lpf = LPF_B;
	else if (frequency < 30000000)
		lpf = LPF_A;

	if (lpf == prev_lpf)
	{
#if DEBUG > 0
		puts("LPF not changed");
#endif
		return;
	}

#if DEBUG > 0
	printf("##################Setting LPF to %d\n", lpf);
#endif

	digitalWrite(LPF_A, LOW);
	digitalWrite(LPF_B, LOW);
	digitalWrite(LPF_C, LOW);
	digitalWrite(LPF_D, LOW);

#if DEBUG > 0
	printf("################ setting %d high\n", lpf);
#endif
	digitalWrite(lpf, HIGH);
	prev_lpf = lpf;
	printf("LPF: selected pin %d for %d Hz\n", lpf, frequency);
}

/* ---- T/R relay and external PTT ------------------------------------------ */

void radio_hw_set_ptt(int on)
{
	digitalWrite(EXT_PTT, on ? HIGH : LOW);
}

void radio_hw_set_tx_relay(int on)
{
	digitalWrite(TX_LINE, on ? HIGH : LOW);
}

/* ---- INA260 power monitor ------------------------------------------------ */

void read_voltage_current(float *voltage, float *current)
{
	uint8_t data_buffer[2]; // Buffer to hold raw register data

	// Explicitly set the register pointer to the voltage register
	if (i2cbb_write_i2c_block_data(INA260_ADDRESS, VOLTAGE_REGISTER, 0, NULL) < 0)
	{
		printf("Error setting voltage register pointer\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}

	// Read the voltage register (2 bytes)
	int e = i2cbb_read_i2c_block_data(INA260_ADDRESS, VOLTAGE_REGISTER, 2, data_buffer);
	if (e != 2)
	{
		printf("Error reading voltage register\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}
	uint16_t raw_voltage = (data_buffer[0] << 8) | data_buffer[1];
	*voltage = raw_voltage * 1.25e-3f; // Convert to volts (1.25 mV per LSB)

	// Explicitly set the register pointer to the current register
	if (i2cbb_write_i2c_block_data(INA260_ADDRESS, CURRENT_REGISTER, 0, NULL) < 0)
	{
		printf("Error setting current register pointer\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}

	// Read the current register (2 bytes)
	e = i2cbb_read_i2c_block_data(INA260_ADDRESS, CURRENT_REGISTER, 2, data_buffer);
	if (e != 2)
	{
		printf("Error reading current register\n");
		*voltage = 0.0f;
		*current = 0.0f;
		return;
	}
	uint16_t raw_current = (data_buffer[0] << 8) | data_buffer[1];

	// Handle saturation or invalid value
	if (raw_current == 0xFFFF)
	{
		printf("Current measurement out of range or invalid\n");
		*current = 0.0f;
	}
	else
	{
		*current = raw_current * 1.25e-3f; // Convert to amps (1.25 mA per LSB)
	}
}

int radio_hw_ina260_configure(void)
{
	uint8_t config_data[2] = {
		(uint8_t)(CONFIG_DEFAULT >> 8),  // MSB
		(uint8_t)(CONFIG_DEFAULT & 0xFF) // LSB
	};
	if (i2cbb_write_i2c_block_data(INA260_ADDRESS, CONFIG_REGISTER, 2, config_data) < 0)
		return -1;
	return 0;
}
