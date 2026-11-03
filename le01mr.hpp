#ifndef E5B1C3D7_2F4A_4B9E_8D06_71A9C4F2B3E8
#define E5B1C3D7_2F4A_4B9E_8D06_71A9C4F2B3E8

#include "serial_io_interface.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// clang-format off
/*
 * Modbus register map of Electrical energy meter LE-01MR v2
 *
 * 0x03 - Read Holding Registers
 * Data reading, according to Modbus RTU protocol, is carried out using
 * the Read Holding Register command (command code 0x03).
 *
 * +-----+---------------------------------------------+----------+----------+-----+------+-------+-------+---------------------+
 * | Lp. | Parameter                                   | Addr HEX | Addr DEC | R/W | Regs | Type  | Unit  | Scale (reg -> phys) |
 * +=====+=============================================+==========+==========+=====+======+=======+=======+=====================+
 * |   1 | Voltage                                     | 0x100    |      256 | R   |    2 | INT32 | V     |               0.001 |
 * |   2 | Current                                     | 0x102    |      258 | R   |    2 | INT32 | A     |               0.001 |
 * |   3 | Active Power                                | 0x104    |      260 | R   |    2 | INT32 | W     |                   1 |
 * |   4 | Apparent Power                              | 0x106    |      262 | R   |    2 | INT32 | VA    |                   1 |
 * |   5 | Reactive Power                              | 0x108    |      264 | R   |    2 | INT32 | Var   |                   1 |
 * |   6 | Frequency                                   | 0x10A    |      266 | R   |    1 | INT16 | Hz    |                 0.1 |
 * |   7 | Power factor                                | 0x10B    |      267 | R   |    1 | INT16 | -     |               0.001 |
 * |   8 | Total consumed (imported) active energy     | 0x10E    |      270 | R   |    2 | INT32 | kWh   |                0.01 |
 * |   9 | Total returned (exported) active energy     | 0x118    |      280 | R   |    2 | INT32 | kWh   |                0.01 |
 * |  10 | Total active energy                         | 0x122    |      290 | R   |    2 | INT32 | kWh   |                0.01 |
 * |  11 | Total consumed (imported) reactive energy   | 0x12C    |      300 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  12 | Total returned (exported) reactive energy   | 0x136    |      310 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  13 | Total reactive energy                       | 0x140    |      320 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  14 | Total reactive energy in the first quadrant | 0x14A    |      330 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  15 | Total reactive energy in the second quad    | 0x154    |      340 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  16 | Total reactive energy in the third quad     | 0x15E    |      350 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  17 | Total reactive energy in the fourth quad    | 0x168    |      360 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  18 | Resettable auxiliary active energy meter    | 0x172    |      370 | R   |    2 | INT32 | kWh   |                0.01 |
 * |  19 | Resettable auxiliary reactive energy meter  | 0x174    |      372 | R   |    2 | INT32 | kvarh |                0.01 |
 * |  20 | Consumed (imported) active power demand     | 0x176    |      374 | R   |    2 | INT32 | W     |                 0.1 |
 * +-----+---------------------------------------------+----------+----------+-----+------+-------+-------+---------------------+
 *
 * 0x06 - Write Single Register
 *
 * +------------------------------+----------+----------+-----+------+
 * | Parameter                    | Addr HEX | Addr DEC | R/W | Regs |
 * +==============================+==========+==========+=====+======+
 * | Auxiliary energy meter reset | 0x2002   |     8194 | R/W |    1 |
 * +------------------------------+----------+----------+-----+------+
 *
 * Values to write:
 *
 * +--------+----------------------------------------+
 * | Value  | Action                                 |
 * +========+========================================+
 * | 0x2000 | Active energy meter reset              |
 * | 0x4000 | Reactive energy meter reset            |
 * | 0x6000 | Active and reactive energy meter reset |
 * +--------+----------------------------------------+
 */
// clang-format on

namespace le01mr {

enum class RegType { Int16, Int32 };

struct ParamDef {
	const char *name;  //!< full name, used in CSV header
	const char *label; //!< short name, used in the console UI
	uint16_t address;
	RegType type;
	const char *unit;
	double scale;	 //!< physical = register * scale
	int decimals;	 //!< number of meaningful decimal places, follows from the scale
	bool barGraph;	 //!< displayed as a bar graph (instantaneous values)
};

//! Indexes into PARAMS and Reading::values
enum Param : size_t {
	Voltage,
	Current,
	ActivePower,
	ApparentPower,
	ReactivePower,
	Frequency,
	PowerFactor,
	ImportedActiveEnergy,
	ExportedActiveEnergy,
	TotalActiveEnergy,
	ImportedReactiveEnergy,
	ExportedReactiveEnergy,
	TotalReactiveEnergy,
	ReactiveEnergyQ1,
	ReactiveEnergyQ2,
	ReactiveEnergyQ3,
	ReactiveEnergyQ4,
	AuxActiveEnergy,
	AuxReactiveEnergy,
	ActivePowerDemand,
	PARAM_COUNT
};

// clang-format off
inline constexpr std::array<ParamDef, PARAM_COUNT> PARAMS = {{
	{"Voltage",                                     "Voltage",                          0x100, RegType::Int32, "V",     0.001, 3, true},
	{"Current",                                     "Current",                          0x102, RegType::Int32, "A",     0.001, 3, true},
	{"Active Power",                                "Active Power",                     0x104, RegType::Int32, "W",     1.0,   0, true},
	{"Apparent Power",                              "Apparent Power",                   0x106, RegType::Int32, "VA",    1.0,   0, true},
	{"Reactive Power",                              "Reactive Power",                   0x108, RegType::Int32, "Var",   1.0,   0, true},
	{"Frequency",                                   "Frequency",                        0x10A, RegType::Int16, "Hz",    0.1,   1, true},
	{"Power factor",                                "Power factor",                     0x10B, RegType::Int16, "-",     0.001, 3, true},
	{"Total consumed (imported) active energy",     "Imported active energy",           0x10E, RegType::Int32, "kWh",   0.01,  2, false},
	{"Total returned (exported) active energy",     "Exported active energy",           0x118, RegType::Int32, "kWh",   0.01,  2, false},
	{"Total active energy",                         "Total active energy",              0x122, RegType::Int32, "kWh",   0.01,  2, false},
	{"Total consumed (imported) reactive energy",   "Imported reactive energy",         0x12C, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Total returned (exported) reactive energy",   "Exported reactive energy",         0x136, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Total reactive energy",                       "Total reactive energy",            0x140, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Total reactive energy in the first quadrant", "Reactive energy Q1",               0x14A, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Total reactive energy in the second quad",    "Reactive energy Q2",               0x154, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Total reactive energy in the third quad",     "Reactive energy Q3",               0x15E, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Total reactive energy in the fourth quad",    "Reactive energy Q4",               0x168, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Resettable auxiliary active energy meter",    "Aux. active energy (resettable)",  0x172, RegType::Int32, "kWh",   0.01,  2, false},
	{"Resettable auxiliary reactive energy meter",  "Aux. reactive energy (resettable)",0x174, RegType::Int32, "kvarh", 0.01,  2, false},
	{"Consumed (imported) active power demand",     "Active power demand (import)",     0x176, RegType::Int32, "W",     0.1,   1, false},
}};
// clang-format on

constexpr uint16_t registerCount (RegType type) { return type == RegType::Int32 ? 2 : 1; }

struct Reading {
	std::chrono::system_clock::time_point timestamp;

	//! Physical values. std::nullopt if a value couldn't be read
	std::array<std::optional<double>, PARAM_COUNT> values;
};

//! A contiguous range of holding registers read with a single 0x03 request
struct RegisterBlock {
	uint16_t start;
	uint16_t count;
};

enum class ReadMode {
	Auto,	 //!< Block, fall back to Grouped if the meter rejects it
	Block,	 //!< as few requests as possible, spanning over unmapped gaps between registers
	Grouped, //!< one request per contiguous group of registers
};

//! LE-01MR v2 rejects (illegal data address) requests for more than 100 registers
inline constexpr uint16_t DEFAULT_MAX_REGISTERS = 100;

class Meter {
  public:
	Meter (SerialIoIf &io, uint8_t slaveId, ReadMode mode, bool swapWords,
		   uint16_t maxRegisters = DEFAULT_MAX_REGISTERS);

	/**
	 * Reads all parameters from the meter.
	 * @throws std::exception if nothing at all could be read
	 */
	Reading read ();

	//! all parameters, merged into blocks of at most maxRegisters (gaps included)
	static std::vector<RegisterBlock> mergedBlocks (uint16_t maxRegisters);

	//! contiguous groups of registers only, no gaps
	static std::vector<RegisterBlock> groupedBlocks ();

  private:
	void readBlock (const RegisterBlock &block, Reading &reading);
	void readMerged (Reading &reading);
	void readGrouped (Reading &reading);

	SerialIoIf &m_io;
	uint8_t m_slaveId;
	ReadMode m_mode;
	bool m_swapWords;
	bool m_blockReadRejected = false;
	std::vector<RegisterBlock> m_blocks;
	std::vector<RegisterBlock> m_groups;
};

} // namespace le01mr

#endif /* E5B1C3D7_2F4A_4B9E_8D06_71A9C4F2B3E8 */
