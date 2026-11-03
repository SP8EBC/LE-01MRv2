#ifndef A7E3F1C2_5B8D_4E2A_9C61_3D0F7B2E8A14
#define A7E3F1C2_5B8D_4E2A_9C61_3D0F7B2E8A14

#include "serial_io_interface.h"
#include <chrono>
#include <modbus.h>

/**
 * Modbus-RTU master implemented using libmodbus.
 */
class SerialIoLibmodbus : public SerialIoIf {
  public:
	//! @throws std::runtime_error if the context cannot be created or the port cannot be opened
	SerialIoLibmodbus (const SerialSettings &settings, std::chrono::milliseconds responseTimeout,
					   bool debug);
	~SerialIoLibmodbus () override;

	SerialIoLibmodbus (const SerialIoLibmodbus &) = delete;
	SerialIoLibmodbus &operator= (const SerialIoLibmodbus &) = delete;

	std::vector<uint16_t> readHoldingRegisters (uint8_t slaveId, uint16_t startRegister,
												uint16_t count) override;

	std::string description () const override;

  private:
	SerialSettings m_settings;
	modbus_t *m_ctx = nullptr;
};

#endif /* A7E3F1C2_5B8D_4E2A_9C61_3D0F7B2E8A14 */
