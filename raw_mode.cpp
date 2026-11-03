#include "raw_mode.hpp"
#include "serial_io_interface.h"
#include <stdexcept>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/fmt/bin_to_hex.h"
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

RawMode::RawMode (uint8_t slaveId, bool skipPrecedingZeros, uint8_t skipPrecedingBytes)
	: m_slaveId{slaveId}, m_skipPrecedingZeros{skipPrecedingZeros},
	  m_skipPrecedingBytes{skipPrecedingBytes}
{
}

std::array<uint8_t, RawMode::REQUEST_SIZE> RawMode::createRequest (uint16_t startRegister,
																	uint16_t count) const
{
	if (count == 0 || count > MAX_REGISTERS) {
		throw std::invalid_argument ("Invalid number of registers to read: " +
									 std::to_string (count));
	}

	std::array<uint8_t, REQUEST_SIZE> out{};

	// slave address
	out[0] = m_slaveId;

	// function code - read holding registers
	out[1] = 0x03;

	// base address of a register cluster to be read from the slave
	out[2] = (startRegister >> 8) & 0xFF;
	out[3] = startRegister & 0xFF;

	// number of registers to be read from slave
	out[4] = (count >> 8) & 0xFF;
	out[5] = count & 0xFF;

	// CRC, low byte first
	const uint16_t crc = RawMode::calculateRtuCrc (std::span{out}.first (6));
	out[6] = crc & 0xFF;
	out[7] = (crc >> 8) & 0xFF;

	return out;
}

std::optional<std::vector<uint16_t>> RawMode::parseResponse (std::span<const uint8_t> in,
															 uint16_t expectedCount) const
{
	std::span<const uint8_t> working = in;

	if (m_skipPrecedingBytes > 0) {
		if (in.size () <= m_skipPrecedingBytes) {
			return std::nullopt;
		}
		working = in.subspan (m_skipPrecedingBytes);
	}
	else if (m_skipPrecedingZeros) {
		size_t i = 0;
		while (i < in.size () && in[i] == 0) {
			i++;
		}
		working = in.subspan (i);
	}

	// the shortest valid frame (exception response) is 5 bytes long
	if (working.size () < 5) {
		return std::nullopt;
	}

	const uint8_t function = working[1];
	size_t frameLength = 0;

	if ((function & 0x80) != 0) {
		// slave address, function | 0x80, exception code, CRC
		frameLength = 5;
	}
	else if (function == 0x03) {
		// slave address, function, byte count, data, CRC
		frameLength = 5u + working[2];
	}
	else {
		throw std::runtime_error (
			fmt::format ("Unexpected function code 0x{:02X} in response: {}",
						 function,
						 spdlog::to_hex (working.begin (), working.end ())));
	}

	if (working.size () < frameLength) {
		return std::nullopt;
	}

	const std::span<const uint8_t> frame = working.first (frameLength);

	// CRC calculated over a whole frame, including the CRC itself, is zero for a valid frame
	if (calculateRtuCrc (frame) != 0) {
		throw std::runtime_error (
			fmt::format ("CRC error in response: {}", spdlog::to_hex (frame.begin (), frame.end ())));
	}

	SPDLOG_TRACE ("Valid response frame: {}", spdlog::to_hex (frame.begin (), frame.end ()));

	if (frame[0] != m_slaveId) {
		throw std::runtime_error (
			fmt::format ("Response from unexpected slave address {} (expected {})", frame[0], m_slaveId));
	}

	if ((function & 0x80) != 0) {
		throw ModbusException (frame[2]);
	}

	if (frame[2] != expectedCount * 2) {
		throw std::runtime_error (fmt::format ("Unexpected byte count in response: {}, expected {}",
											   frame[2],
											   expectedCount * 2));
	}

	std::vector<uint16_t> registersValues (expectedCount);
	for (size_t i = 0; i < expectedCount; i++) {
		registersValues[i] = static_cast<uint16_t> (frame[3 + 2 * i] << 8 | frame[4 + 2 * i]);
	}

	return registersValues;
}

uint16_t RawMode::calculateRtuCrc (std::span<const uint8_t> in)
{
	uint16_t crc = 0xFFFF;

	for (const uint8_t byte : in) {
		crc = calculateStreamRtuCrc (crc, byte);
	}

	// Note, this number has low and high bytes swapped, so use it accordingly (or swap bytes)
	return crc;
}

uint16_t RawMode::calculateStreamRtuCrc (uint16_t previousCrc, uint8_t currentData)
{
	previousCrc ^= (uint16_t)currentData; // XOR byte into least sig. byte of crc

	for (int i = 0; i < 8; ++i) { // Loop over each bit
		if (previousCrc & 1) {	  // If the LSB is set shift right and XOR 0xA001
			previousCrc = (previousCrc >> 1) ^ 0xA001;
		}
		else { // Else LSB is not set, just shift right
			previousCrc = (previousCrc >> 1);
		}
	}

	return previousCrc;
}
