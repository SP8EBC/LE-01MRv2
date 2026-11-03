#ifndef B4D2EC22_6719_4E6C_8CC8_17AA463549BA
#define B4D2EC22_6719_4E6C_8CC8_17AA463549BA

#include <cstdint>
#include <span>
#include <tuple>


class RawMode {
  public:
	RawMode (uint16_t slaveId, bool skipPrecedingZeros, uint8_t skipPrecedingBytes);

	void createRequest (std::span<uint8_t> out);

	std::array<uint16_t, 16> parseResponse (const std::span<uint8_t> &in);

	static uint16_t calculateRtuCrc (const std::span<uint8_t> &in);

	static uint16_t calculateStreamRtuCrc(uint16_t previousCrc, uint8_t currentData);

  private:
	uint16_t m_slaveId;
	bool m_skipPrecedingZeros;
	uint8_t m_skipPrecedingBytes;
};

#endif /* B4D2EC22_6719_4E6C_8CC8_17AA463549BA */
