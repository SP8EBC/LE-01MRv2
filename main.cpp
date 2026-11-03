#include <boost/program_options.hpp>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <modbus.h>
#include <string>

#include "raw_mode.hpp"
#include "serial_io_raw.hpp"

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/fmt/bin_to_hex.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

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

//!< serial port to use for communication with Modbus-RTU slave
std::string portName;

//!< Where to store output file with weather frame
std::string outputFileName;

//!< a delay in microseconds between setting RTS line and starting transmission
int modbusSetRtsDelay = 50000;

//!< RTS line state while it is set to active
int modbusSetRtuMode = MODBUS_RTU_RTS_UP;

//!< If true will set modbus_rtu_set_serial_mode to MODBUS_RTU_RS485, otherwise to MODBUS_RTU_RS232
bool modbusSetSerialMode485 = false;

bool rawModeEnabled = false;

uint8_t rawSkipBytes = 0;

bool rawSkipZeros = false;

void parseArgs (int argc, char *argv[])
{
	boost::program_options::options_description optionsDescription ("");

	// ----------------
	// GENERAL OPTIONS
	// ----------------
	boost::program_options::options_description generalOptions ("General Options");
	boost::program_options::options_description_easy_init port = generalOptions.add_options ();
	port ("port,P",
		  boost::program_options::value<std::string> (&portName),
		  " : Serial port used for communication");
	boost::program_options::options_description_easy_init mode485 = generalOptions.add_options ();
	mode485 ("mode-485,4", " : Set to use  MODBUS_RTU_RS485 instead of _RS232 ");
	boost::program_options::options_description_easy_init raw = generalOptions.add_options ();
	raw ("raw,R", " : use low level function for communication ");
	boost::program_options::options_description_easy_init output = generalOptions.add_options ();
	output ("out,O",
			boost::program_options::value<std::string> (&outputFileName),
			" : Path to output CSV file");
	optionsDescription.add (generalOptions);

	// -----------------
	// RTS OPTIONS
	// -----------------
	boost::program_options::options_description rtsOptions ("RTS line Options");
	boost::program_options::options_description_easy_init rtsDelay = rtsOptions.add_options ();
	rtsDelay ("rts-delay",
			  boost::program_options::value<int> (&modbusSetRtsDelay),
			  " : Delay in microseconds between setting RTS line and starting transmission. By "
			  "default: 50,000");
	boost::program_options::options_description_easy_init rtsMode = rtsOptions.add_options ();
	rtsDelay (
		"rts-mode",
		boost::program_options::value<int> (&modbusSetRtuMode),
		" : State of RTS line in active state: 0 - don't toggle, 1 - RTS_UP, 2 - RTS_DOWN. By "
		"default: RTS_UP");

	optionsDescription.add (rtsOptions);

	// -----------------
	// RAW MODE OPTIONS
	// -----------------
	boost::program_options::options_description rawOptions ("Raw Mode Options");
	boost::program_options::options_description_easy_init skipZeros = rawOptions.add_options ();
	skipZeros ("raw-skip-zeros",
			   " : Skips all preceding zeros before first non-zero byte of a response.");
	boost::program_options::options_description_easy_init skipBytes = rawOptions.add_options ();
	skipBytes ("raw-skip-bytes",
			   boost::program_options::value<uint8_t> (&rawSkipBytes),
			   " : Skips specified number of bytes from the begining of a response.");

	optionsDescription.add (rawOptions);

	// PARSING
	boost::program_options::variables_map odVariablesMap;
	try {
		boost::program_options::store (
			boost::program_options::parse_command_line (argc, argv, optionsDescription),
			odVariablesMap);
		boost::program_options::notify (odVariablesMap);
	}
	catch (boost::wrapexcept<boost::program_options::unknown_option> &ex) {
		SPDLOG_ERROR ("Missing or wrong parameter!");
		std::cout << ex.what () << std::endl << optionsDescription << std::endl;
		exit (-2);
	}

	if (portName.size () == 0) {
		SPDLOG_ERROR ("Serial port not specified!");
		std::cout << optionsDescription << std::endl;
		exit (-3);
	}

	if (odVariablesMap.count ("mode-485")) {
		modbusSetSerialMode485 = true;
	}

	if (odVariablesMap.count ("raw")) {
		rawModeEnabled = true;
	}

	if (odVariablesMap.count ("raw-skip-zeros")) {
		if (rawSkipBytes > 0) {
			SPDLOG_ERROR (
				"Conflicting settings! Cannot use raw-skip-zeros and raw-skip-bytes together");
			exit (-4);
		}
		else {
			rawSkipZeros = true;
			SPDLOG_DEBUG ("rawSkipZeros enabled");
		}
	}
}

int main (int argc, char *argv[])
{
	spdlog::set_level (spdlog::level::debug);
	SPDLOG_INFO ("============ MODBUS2APRS  =============");

	const int REMOTE_ID = 1;
	modbus_t *ctx = nullptr;
	std::array<uint16_t, 16> tab_reg = {0u};

	parseArgs (argc, argv);

	SPDLOG_DEBUG ("modbusSetRtsDelay: {}", modbusSetRtsDelay);
	SPDLOG_DEBUG ("modbusSetRtuMode: {}", modbusSetRtuMode);

	SerialIoRaw serialIoRaw{portName,
							modbusSetRtsDelay,
							modbusSetSerialMode485,
							rawSkipBytes,
							rawSkipZeros};

	if (rawModeEnabled) {

		SPDLOG_INFO ("RTU communication will be done using raw functions.");
		SPDLOG_INFO ("set of --raw-xxxxxxx parameters will be applied now.");
	}
	else {
		ctx = modbus_new_rtu (portName.c_str (), 9600, 'N', 8, 2);
		if (ctx == NULL) {
			SPDLOG_ERROR ("Unable to create the libmodbus context\n");
			return -1;
		}

		modbus_set_debug (ctx, TRUE);

		int mode;

		if (modbusSetSerialMode485) {
			mode = MODBUS_RTU_RS485;
		}
		else {
			mode = MODBUS_RTU_RS232;
		}

		if (modbus_rtu_set_serial_mode (ctx, mode) == -1) {
			SPDLOG_ERROR ("Set serial mode failed: {}\n", modbus_strerror (errno));
			SPDLOG_ERROR ("Maybe MODBUS_RTU_RS485 is not supported?");
		}

		if (modbus_rtu_set_rts (ctx, modbusSetRtuMode) == -1) {
			SPDLOG_ERROR ("Set rts failed: {}\n", modbus_strerror (errno));
		}

		modbus_rtu_set_rts_delay (ctx, modbusSetRtsDelay);
		if (modbus_connect (ctx) == -1) {
			SPDLOG_ERROR ("Connection failed: {}\n", modbus_strerror (errno));
			modbus_free (ctx);
			return -1;
		}

		modbus_set_slave (ctx, REMOTE_ID);
	}

	// initialize console sudo-GUI here. display this in similar way
	// as HTOP displays CPU or memory usage. A horizontal bar graph with description on the left a
	// value on the right. only: Voltage, Current, Active Power, Apparent Power, Reactive Power,
	// Frequency and Power Factor should be displayed using bar graph. The rest only as a 
	// label - value with unit

	// open CSV output file for appending. if a user didn't specified an output file
	// in a console parameter, create new one with a filename generated from current date and time

	while (true) {

		// sleep for one second
		sleep (1);

		// read data from Electrical energy meter LE-01MR v2
		if (rawModeEnabled) {
			// request all holding registers listed in the comment on top of the file
		}
		else {
			// request all holding registers listed in the comment on top of the file
		}

		// decode values from raw register value to real units

		// update display

		// store all registers in CSV file
	}

	if (rawModeEnabled) {
		;
	}
	else {
		if (ctx != nullptr) {
			modbus_free (ctx);
		}
	}

	return 0;
}
