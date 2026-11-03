#ifndef B4D2EC22_6719_4E6C_8CC8_17AA463549BA
#define B4D2EC22_6719_4E6C_8CC8_17AA463549BA

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/**
 * Low level Modbus-RTU frame builder / parser for function 0x03 (Read Holding Registers).
 */
class RawMode {
  public:
	static constexpr size_t REQUEST_SIZE = 8;

	//! Maximum number of registers which may be read with a single 0x03 request
	static constexpr uint16_t MAX_REGISTERS = 125;

	RawMode (uint8_t slaveId, bool skipPrecedingZeros, uint8_t skipPrecedingBytes);

	std::array<uint8_t, REQUEST_SIZE> createRequest (uint16_t startRegister, uint16_t count) const;

	/**
	 * Tries to parse a response collected so far.
	 *
	 * @return std::nullopt if the frame is not complete yet, registers values otherwise
	 * @throws ModbusException if the slave responded with an exception frame
	 * @throws std::runtime_error on CRC error, wrong slave address, unexpected function or length
	 */
	std::optional<std::vector<uint16_t>> parseResponse (std::span<const uint8_t> in,
														uint16_t expectedCount) const;

	static uint16_t calculateRtuCrc (std::span<const uint8_t> in);

	static uint16_t calculateStreamRtuCrc (uint16_t previousCrc, uint8_t currentData);

  private:
	uint8_t m_slaveId;
	bool m_skipPrecedingZeros;
	uint8_t m_skipPrecedingBytes;
};

#endif /* B4D2EC22_6719_4E6C_8CC8_17AA463549BA */
