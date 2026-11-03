#ifndef C11D1360_4672_4779_82D2_1B8881C12248
#define C11D1360_4672_4779_82D2_1B8881C12248

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * Settings of the serial line used for Modbus-RTU communication.
 */
struct SerialSettings {
	std::string port;
	int baudrate = 9600;
	char parity = 'N'; //!< 'N', 'E' or 'O'
	int dataBits = 8;
	int stopBits = 1;
	bool rs485 = false; //!< MODBUS_RTU_RS485 instead of MODBUS_RTU_RS232
	int rtsMode = 1;	//!< 0 - don't toggle, 1 - RTS_UP, 2 - RTS_DOWN
	int rtsDelayUs = 50000;

	std::string frameDescription () const
	{
		return std::to_string (baudrate) + " " + std::to_string (dataBits) + parity +
			   std::to_string (stopBits);
	}
};

/**
 * Thrown when a slave answered with a Modbus exception response (function code | 0x80).
 */
class ModbusException : public std::runtime_error {
  public:
	static constexpr uint8_t ILLEGAL_FUNCTION = 0x01;
	static constexpr uint8_t ILLEGAL_DATA_ADDRESS = 0x02;
	static constexpr uint8_t ILLEGAL_DATA_VALUE = 0x03;

	explicit ModbusException (uint8_t code) : std::runtime_error (describe (code)), m_code{code} {}

	uint8_t code () const noexcept { return m_code; }

	static std::string describe (uint8_t code)
	{
		const char *text = "unknown exception";
		switch (code) {
		case 0x01: text = "illegal function"; break;
		case 0x02: text = "illegal data address"; break;
		case 0x03: text = "illegal data value"; break;
		case 0x04: text = "slave device failure"; break;
		case 0x05: text = "acknowledge"; break;
		case 0x06: text = "slave device busy"; break;
		case 0x07: text = "negative acknowledge"; break;
		case 0x08: text = "memory parity error"; break;
		case 0x0A: text = "gateway path unavailable"; break;
		case 0x0B: text = "gateway target device failed to respond"; break;
		}
		return "Modbus exception 0x" + std::string{"0123456789ABCDEF"[code >> 4]} +
			   std::string{"0123456789ABCDEF"[code & 0xF]} + ": " + text;
	}

  private:
	uint8_t m_code;
};

/**
 * Abstract Modbus-RTU master. Implementations throw ModbusException when the slave
 * answers with an exception and std::runtime_error (or derived) on any other failure
 * (timeout, CRC error, I/O error...).
 */
class SerialIoIf {
  public:
	virtual ~SerialIoIf () = default;

	//! Function 0x03 - Read Holding Registers
	virtual std::vector<uint16_t> readHoldingRegisters (uint8_t slaveId, uint16_t startRegister,
														uint16_t count) = 0;

	//! Short human readable description of the backend, used in UI
	virtual std::string description () const = 0;
};

#endif /* C11D1360_4672_4779_82D2_1B8881C12248 */
