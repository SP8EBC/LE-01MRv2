#ifndef CADFA9C9_1134_4451_A988_EA1092BC7537
#define CADFA9C9_1134_4451_A988_EA1092BC7537

#include "serial/serial.h"
#include "serial_io_interface.h"
#include <chrono>
#include <span>
#include <string>

/**
 * Modbus-RTU master implemented directly on top of the serial port, without libmodbus.
 */
class SerialIoRaw : public SerialIoIf {
  public:
	SerialIoRaw (const SerialSettings &settings, uint8_t rawSkipBytes, bool rawSkipZeros,
				 std::chrono::milliseconds responseTimeout);

	std::vector<uint16_t> readHoldingRegisters (uint8_t slaveId, uint16_t startRegister,
												uint16_t count) override;

	std::string description () const override;

  private:
	void ensureOpen ();
	void transmit (std::span<const uint8_t> frame);

	SerialSettings m_settings;
	uint8_t m_rawSkipBytes;
	bool m_rawSkipZeros;
	std::chrono::milliseconds m_responseTimeout;
	serial::Serial m_rawSerialPort;
};

#endif /* CADFA9C9_1134_4451_A988_EA1092BC7537 */
