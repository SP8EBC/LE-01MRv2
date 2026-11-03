#ifndef CADFA9C9_1134_4451_A988_EA1092BC7537
#define CADFA9C9_1134_4451_A988_EA1092BC7537

#include "serial/serial.h"
#include "serial_io_interface.h"
#include <string>

class SerialIoRaw : public SerialIoIf {
  public:
	SerialIoRaw (std::string &portName, int modbusRtsDelay, bool modbusSerialMode485,
				 uint8_t rawSkipBytes, bool rawSkipZeros);

	virtual void requestRegisters (uint16_t slaveid, uint16_t startregister, uint8_t num,
								   std::array<uint16_t, 16> &out);

  private:
	std::string m_portName;
	int m_modbusRtuDelay;
	bool m_modbusSerialMode485;
	uint8_t m_rawSkipBytes;
	bool m_rawSkipZeros;
	serial::Serial m_rawSerialPort;
};

#endif /* CADFA9C9_1134_4451_A988_EA1092BC7537 */
