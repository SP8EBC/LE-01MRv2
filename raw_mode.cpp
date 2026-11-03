#include "raw_mode.hpp"
#include <exception>
#include <stdexcept>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/fmt/bin_to_hex.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

RawMode::RawMode (uint16_t slaveId, bool skipPrecedingZeros, uint8_t skipPrecedingBytes)
	: m_slaveId{slaveId}, m_skipPrecedingZeros{skipPrecedingZeros},
	  m_skipPrecedingBytes{skipPrecedingBytes}
{
}

void RawMode::createRequest (std::span<uint8_t> out)
{
	// put the slave address
	out[0] = (uint8_t)m_slaveId;

	// put the function code - read holding registers
	out[1] = 0x03;

	// put the base address of a register cluster to be read from the slave
	out[2] = 0;
	out[3] = 0;

	// put the numbers of register to be read from slave
	out[4] = 0x00; // all in all modbus RTU can transfer no more than 125 at once
	out[5] = 8;

	// calculate the CRC from the content
	const uint16_t crc = RawMode::calculateRtuCrc (out.subspan (0, out.size () - 2));

	// append the crc value
	out[6] = crc & 0xFF;
	out[7] = (crc & 0xFF00) >> 8;
}

std::array<uint16_t, 16> RawMode::parseResponse (const std::span<uint8_t> &in)
{
	// SensorData out = {0u};

	std::array<uint16_t, 16> registersValues = {0u};

	std::span<uint8_t> working;

	if (m_skipPrecedingBytes > 0) {
		SPDLOG_INFO ("Skipping first {} bytes", m_skipPrecedingBytes);
		working = in.subspan (m_skipPrecedingBytes, in.size () - m_skipPrecedingBytes);
	}
	else if (m_skipPrecedingZeros) {
		int i = 0;

		for (uint8_t current : in) {
			if (current != 0) {
				break;
			}
			else {
				i++;
			}
		}

		working = in.subspan (i, in.size () - i);
		SPDLOG_INFO ("Walked over first {} zeros. First non zero byte is 0x{:X}", i, working[0]);
	}
	else {
		working = in;
	}

	uint16_t previousCrc = 0xFFFF;

	uint8_t len = 0;

	// checking CRC
	for (const uint8_t byte : working) {
		previousCrc = RawMode::calculateStreamRtuCrc (previousCrc, byte);

		len++;

		if (previousCrc == 0x00) {
			const uint16_t correctCrc = working[len - 2] | (working[len - 1] << 8);
			SPDLOG_INFO ("received CRC 0x{:X} is correct. True response length is: {}",
						 correctCrc,
						 len);
			break;
		}
	}

	if (previousCrc != 0x00) {
		SPDLOG_ERROR ("CRC error!!!!");
		throw std::runtime_error ("CRC error");
	}

	// iterator through input table and registers table
	int j = 3;

	// fetch slave address
	uint8_t data = working[0];

	// TODO: store slave address
	const uint8_t slave_address_from_frame = data;

	// fetch function code
	data = working[1];

	// if the exception flag is set
	if ((data & 0x80) > 0) {
		// get error code
		data = working[2];

		// parse the exception value
		static char whatMessage[64];
		snprintf (whatMessage,
				  64,
				  "Modbus RTU exception flag, [0]: 0x%X [1]: 0x%X [2]: 0x%X",
				  in[0],
				  in[1],
				  in[2]);

		throw std::runtime_error (whatMessage);
	}

	// check if the function code is correct or not
	else if (data == 0x03 || data == 0x04) {

		// check if this is an answer from the slave we expect
		if (slave_address_from_frame == m_slaveId) {
			// fetch the function result lenght in bytes
			data = working[2];

			// store amount of registers in this response
			const uint8_t number_of_registers = data / 2;

			// get all registers values
			for (int i = 0; i < number_of_registers; i++) {
				registersValues[i] = working[j] << 8 | working[j + 1];

				// moving to next 16bit word with next register
				j += 2;
			}
		}
		else {
			throw std::runtime_error ("Unexpected slave address");
		}
	}
	else {
		// if not exit with an error as this isn't
		// correct parser for this modbus function
		throw std::runtime_error ("Unknown Modbus function");
	}

	return registersValues;
}

uint16_t RawMode::calculateRtuCrc (const std::span<uint8_t> &in)
{
	uint16_t crc = 0xFFFF;

	for (int pos = 0; pos < in.size (); pos++) {
		crc ^= (uint16_t)in[pos]; // XOR byte into least sig. byte of crc

		for (int i = 8; i != 0; i--) { // Loop over each bit
			if ((crc & 0x0001) != 0) { // If the LSB is set
				crc >>= 1;			   // Shift right and XOR 0xA001
				crc ^= 0xA001;
			}
			else		   // Else LSB is not set
				crc >>= 1; // Just shift right
		}
	}
	// Note, this number has low and high bytes swapped, so use it accordingly (or swap bytes)
	return crc;
}

uint16_t RawMode::calculateStreamRtuCrc (uint16_t previousCrc, uint8_t currentData)
{
	int i;

	previousCrc ^= (uint16_t)currentData;
	for (i = 0; i < 8; ++i) {
		if (previousCrc & 1) {
			previousCrc = (previousCrc >> 1);
			previousCrc = (previousCrc) ^ 0xA001;
		}
		else
			previousCrc = (previousCrc >> 1);
	}

	return previousCrc;
}