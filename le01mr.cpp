#include "le01mr.hpp"
#include <algorithm>
#include <exception>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

namespace le01mr {

namespace {

std::array<ParamDef, PARAM_COUNT> sortedByAddress ()
{
	std::array<ParamDef, PARAM_COUNT> sorted = PARAMS;
	std::sort (sorted.begin (), sorted.end (), [] (const ParamDef &a, const ParamDef &b) {
		return a.address < b.address;
	});
	return sorted;
}

} // namespace

Meter::Meter (SerialIoIf &io, uint8_t slaveId, ReadMode mode, bool swapWords,
			  uint16_t maxRegisters)
	: m_io{io}, m_slaveId{slaveId}, m_mode{mode}, m_swapWords{swapWords},
	  m_blocks{mergedBlocks (maxRegisters)}, m_groups{groupedBlocks ()}
{
}

std::vector<RegisterBlock> Meter::mergedBlocks (uint16_t maxRegisters)
{
	std::vector<RegisterBlock> blocks;

	for (const auto &param : sortedByAddress ()) {
		const uint16_t end = param.address + registerCount (param.type);

		// a parameter is never split between two requests
		if (!blocks.empty () && end - blocks.back ().start <= maxRegisters) {
			blocks.back ().count = end - blocks.back ().start;
		}
		else {
			blocks.push_back ({param.address, registerCount (param.type)});
		}
	}

	return blocks;
}

std::vector<RegisterBlock> Meter::groupedBlocks ()
{
	std::vector<RegisterBlock> blocks;

	for (const auto &param : sortedByAddress ()) {
		const uint16_t count = registerCount (param.type);

		if (!blocks.empty () && blocks.back ().start + blocks.back ().count == param.address) {
			blocks.back ().count += count;
		}
		else {
			blocks.push_back ({param.address, count});
		}
	}

	return blocks;
}

void Meter::readMerged (Reading &reading)
{
	for (const auto &block : m_blocks) {
		readBlock (block, reading);
	}
}

void Meter::readBlock (const RegisterBlock &block, Reading &reading)
{
	const std::vector<uint16_t> regs = m_io.readHoldingRegisters (m_slaveId, block.start, block.count);

	for (size_t i = 0; i < PARAM_COUNT; i++) {
		const ParamDef &param = PARAMS[i];
		const uint16_t count = registerCount (param.type);

		if (param.address < block.start || param.address + count > block.start + block.count) {
			continue;
		}

		const size_t offset = param.address - block.start;
		int32_t raw = 0;

		if (param.type == RegType::Int16) {
			raw = static_cast<int16_t> (regs[offset]);
		}
		else {
			uint16_t high = regs[offset];
			uint16_t low = regs[offset + 1];
			if (m_swapWords) {
				std::swap (high, low);
			}
			raw = static_cast<int32_t> ((static_cast<uint32_t> (high) << 16) | low);
		}

		reading.values[i] = raw * param.scale;
	}
}

void Meter::readGrouped (Reading &reading)
{
	size_t succeeded = 0;
	std::exception_ptr lastError;

	for (const auto &block : m_groups) {
		try {
			readBlock (block, reading);
			succeeded++;
		}
		catch (const ModbusException &ex) {
			// the meter is alive but rejected this particular group, try remaining ones
			SPDLOG_WARN ("Reading registers 0x{:X}..0x{:X} failed: {}",
						 block.start,
						 block.start + block.count - 1,
						 ex.what ());
			lastError = std::current_exception ();
		}
		// any other error (timeout, CRC, I/O) aborts the whole cycle, the meter is most likely
		// not responding at all and there is no point in waiting for each group to time out
	}

	if (succeeded == 0 && lastError) {
		std::rethrow_exception (lastError);
	}
}

Reading Meter::read ()
{
	Reading reading{};
	reading.timestamp = std::chrono::system_clock::now ();

	const bool useBlock =
		m_mode == ReadMode::Block || (m_mode == ReadMode::Auto && !m_blockReadRejected);

	if (useBlock) {
		try {
			readMerged (reading);
			return reading;
		}
		catch (const ModbusException &ex) {
			const bool rejected = ex.code () == ModbusException::ILLEGAL_DATA_ADDRESS ||
								  ex.code () == ModbusException::ILLEGAL_DATA_VALUE;
			if (m_mode == ReadMode::Auto && rejected) {
				SPDLOG_WARN ("Meter rejected reading registers in {} block(s) ({}), switching to "
							 "{} requests without gaps. Consider lowering --max-registers",
							 m_blocks.size (),
							 ex.what (),
							 m_groups.size ());
				m_blockReadRejected = true;
				reading.values = {};
			}
			else {
				throw;
			}
		}
	}

	readGrouped (reading);
	return reading;
}

} // namespace le01mr
