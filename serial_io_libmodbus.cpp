#include "serial_io_libmodbus.hpp"
#include <cerrno>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

SerialIoLibmodbus::SerialIoLibmodbus (const SerialSettings &settings,
									  std::chrono::milliseconds responseTimeout, bool debug)
	: m_settings{settings}
{
	m_ctx = modbus_new_rtu (m_settings.port.c_str (),
							m_settings.baudrate,
							m_settings.parity,
							m_settings.dataBits,
							m_settings.stopBits);
	if (m_ctx == nullptr) {
		throw std::runtime_error (std::string ("Unable to create the libmodbus context: ") +
								  modbus_strerror (errno));
	}

	modbus_set_debug (m_ctx, debug ? TRUE : FALSE);

	const int mode = m_settings.rs485 ? MODBUS_RTU_RS485 : MODBUS_RTU_RS232;

	if (modbus_rtu_set_serial_mode (m_ctx, mode) == -1) {
		SPDLOG_WARN ("Set serial mode failed: {}", modbus_strerror (errno));
		SPDLOG_WARN ("Maybe MODBUS_RTU_RS485 is not supported?");
	}

	if (modbus_rtu_set_rts (m_ctx, m_settings.rtsMode) == -1) {
		SPDLOG_WARN ("Set rts failed: {}", modbus_strerror (errno));
	}

	modbus_rtu_set_rts_delay (m_ctx, m_settings.rtsDelayUs);

	const auto timeoutMs = static_cast<uint32_t> (responseTimeout.count ());
	modbus_set_response_timeout (m_ctx, timeoutMs / 1000, (timeoutMs % 1000) * 1000);

	if (modbus_connect (m_ctx) == -1) {
		const std::string error = modbus_strerror (errno);
		modbus_free (m_ctx);
		m_ctx = nullptr;
		throw std::runtime_error ("Connection failed: " + error);
	}
}

SerialIoLibmodbus::~SerialIoLibmodbus ()
{
	if (m_ctx != nullptr) {
		modbus_close (m_ctx);
		modbus_free (m_ctx);
	}
}

std::string SerialIoLibmodbus::description () const
{
	return m_settings.port + " " + m_settings.frameDescription () + ", libmodbus" +
		   (m_settings.rs485 ? ", RS485" : "");
}

std::vector<uint16_t> SerialIoLibmodbus::readHoldingRegisters (uint8_t slaveId,
															   uint16_t startRegister,
															   uint16_t count)
{
	std::vector<uint16_t> registers (count);

	modbus_set_slave (m_ctx, slaveId);

	const int result = modbus_read_registers (m_ctx, startRegister, count, registers.data ());

	if (result == -1) {
		const int error = errno;

		if (error >= EMBXILFUN && error <= EMBXGTAR) {
			throw ModbusException (static_cast<uint8_t> (error - MODBUS_ENOBASE));
		}

		// discard any leftovers so they won't break the next transaction
		modbus_flush (m_ctx);
		throw std::runtime_error (modbus_strerror (error));
	}

	if (result != count) {
		throw std::runtime_error (
			fmt::format ("Received {} registers instead of {}", result, count));
	}

	return registers;
}
