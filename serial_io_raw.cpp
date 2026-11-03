#include "serial_io_raw.hpp"
#include "raw_mode.hpp"
#include <algorithm>
#include <thread>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/fmt/bin_to_hex.h"
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

namespace {

serial::parity_t toParity (char parity)
{
	switch (parity) {
	case 'E': return serial::parity_t::parity_even;
	case 'O': return serial::parity_t::parity_odd;
	default: return serial::parity_t::parity_none;
	}
}

serial::stopbits_t toStopBits (int stopBits)
{
	return stopBits == 2 ? serial::stopbits_t::stopbits_two : serial::stopbits_t::stopbits_one;
}

serial::bytesize_t toByteSize (int dataBits)
{
	switch (dataBits) {
	case 5: return serial::bytesize_t::fivebits;
	case 6: return serial::bytesize_t::sixbits;
	case 7: return serial::bytesize_t::sevenbits;
	default: return serial::bytesize_t::eightbits;
	}
}

//! Single read call blocks no longer than this, so the overall response timeout is honored
constexpr uint32_t POLL_TIMEOUT_MS = 20;

} // namespace

SerialIoRaw::SerialIoRaw (const SerialSettings &settings, uint8_t rawSkipBytes, bool rawSkipZeros,
						  std::chrono::milliseconds responseTimeout)
	: m_settings{settings}, m_rawSkipBytes{rawSkipBytes}, m_rawSkipZeros{rawSkipZeros},
	  m_responseTimeout{responseTimeout},
	  m_rawSerialPort{"",
					  static_cast<uint32_t> (settings.baudrate),
					  serial::Timeout (serial::Timeout::max (), POLL_TIMEOUT_MS, 0, 1000, 2),
					  toByteSize (settings.dataBits),
					  toParity (settings.parity),
					  toStopBits (settings.stopBits),
					  serial::flowcontrol_t::flowcontrol_none}
{
	m_rawSerialPort.setPort (m_settings.port);
}

std::string SerialIoRaw::description () const
{
	return m_settings.port + " " + m_settings.frameDescription () + ", raw" +
		   (m_settings.rs485 ? ", RS485" : "");
}

void SerialIoRaw::ensureOpen ()
{
	if (!m_rawSerialPort.isOpen ()) {
		SPDLOG_INFO ("Opening serial port: {}, {}", m_settings.port, m_settings.frameDescription ());
		m_rawSerialPort.open ();
	}
}

void SerialIoRaw::transmit (std::span<const uint8_t> frame)
{
	const bool toggleRts = m_settings.rs485 && m_settings.rtsMode != 0;
	const bool rtsActive = m_settings.rtsMode == 1;

	if (toggleRts) {
		m_rawSerialPort.setRTS (rtsActive);
		std::this_thread::sleep_for (std::chrono::microseconds (m_settings.rtsDelayUs));
	}

	m_rawSerialPort.write (frame.data (), frame.size ());

	if (toggleRts) {
		// wait until all data are physically sent (tcdrain), then release the bus
		m_rawSerialPort.flush ();
		std::this_thread::sleep_for (std::chrono::microseconds (m_settings.rtsDelayUs));
		m_rawSerialPort.setRTS (!rtsActive);
	}
}

std::vector<uint16_t> SerialIoRaw::readHoldingRegisters (uint8_t slaveId, uint16_t startRegister,
														 uint16_t count)
{
	const RawMode rawMode{slaveId, m_rawSkipZeros, m_rawSkipBytes};
	const auto request = rawMode.createRequest (startRegister, count);

	try {
		ensureOpen ();

		// drop any garbage left from previous, possibly failed, transaction
		m_rawSerialPort.flushInput ();

		SPDLOG_DEBUG ("rawRequest: {}", spdlog::to_hex (request));
		transmit (request);

		std::vector<uint8_t> response;
		response.reserve (256);

		const auto deadline = std::chrono::steady_clock::now () + m_responseTimeout;

		while (true) {
			auto parsed = rawMode.parseResponse (response, count);
			if (parsed.has_value ()) {
				SPDLOG_DEBUG ("rawResponse: {}", spdlog::to_hex (response));
				return std::move (*parsed);
			}

			if (std::chrono::steady_clock::now () >= deadline) {
				if (response.empty ()) {
					throw std::runtime_error ("Response timeout, no data received");
				}
				throw std::runtime_error (
					fmt::format ("Response timeout, incomplete frame ({} bytes): {}",
								 response.size (),
								 spdlog::to_hex (response)));
			}

			if (response.size () > 1024) {
				throw std::runtime_error ("Too much data received without a valid frame");
			}

			std::array<uint8_t, 256> chunk;
			const size_t available = m_rawSerialPort.available ();
			const size_t toRead = std::clamp<size_t> (available, 1, chunk.size ());
			const size_t received = m_rawSerialPort.read (chunk.data (), toRead);
			response.insert (response.end (), chunk.begin (), chunk.begin () + received);
		}
	}
	catch (const serial::IOException &ex) {
		// the port might have disappeared (USB adapter unplugged). Reopen it next time
		m_rawSerialPort.close ();
		throw std::runtime_error (std::string ("Serial port I/O error: ") + ex.what ());
	}
	catch (const serial::SerialException &ex) {
		m_rawSerialPort.close ();
		throw std::runtime_error (std::string ("Serial port error: ") + ex.what ());
	}
	catch (const serial::PortNotOpenedException &ex) {
		throw std::runtime_error (std::string ("Serial port error: ") + ex.what ());
	}
}
