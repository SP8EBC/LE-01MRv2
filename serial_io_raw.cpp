#include "serial_io_raw.hpp"
#include "raw_mode.hpp"

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/fmt/bin_to_hex.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

SerialIoRaw::SerialIoRaw (std::string &portName, int modbusRtsDelay, bool modbusSerialMode485,
						  uint8_t rawSkipBytes, bool rawSkipZeros)
	: m_portName{portName}, m_modbusRtuDelay{modbusRtsDelay},
	  m_modbusSerialMode485{modbusSerialMode485}, m_rawSkipBytes{rawSkipBytes},
	  m_rawSkipZeros{rawSkipZeros}, m_rawSerialPort{"",
													9600,
													serial::Timeout::simpleTimeout (123),
													serial::bytesize_t::eightbits,
													serial::parity_t::parity_none,
													serial::stopbits_t::stopbits_two,
													serial::flowcontrol_t::flowcontrol_none}
{ m_rawSerialPort.setPort (m_portName); }

void SerialIoRaw::requestRegisters (uint16_t slaveid, uint16_t startregister, uint8_t num,
									std::array<uint16_t, 16> &out)
{
	std::array<uint8_t, 8> rawRequest = {0u};
	std::array<uint8_t, 32> rawResponse = {0u};
	RawMode rawMode{slaveid, m_rawSkipZeros, m_rawSkipBytes};

	SPDLOG_INFO ("Opening serial port: {}, speed: {}, stop bits: {}",
				 m_rawSerialPort.getPort (),
				 m_rawSerialPort.getBaudrate (),
				 m_rawSerialPort.getStopbits ());
	m_rawSerialPort.open ();

	rawMode.createRequest (rawRequest);

	SPDLOG_DEBUG ("rawRequest: {}", spdlog::to_hex (rawRequest));

	m_rawSerialPort.write (rawRequest.data (), rawRequest.size ());

	const size_t readResult = m_rawSerialPort.read (rawResponse.data (), rawResponse.size ());

	SPDLOG_INFO ("number of bytes received: {}", readResult);
	SPDLOG_DEBUG ("rawResponse: {}", spdlog::to_hex (rawResponse));

	out = rawMode.parseResponse (rawResponse);

	m_rawSerialPort.close ();
}