#include <boost/program_options.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "console_ui.hpp"
#include "csv_writer.hpp"
#include "le01mr.hpp"
#include "sftp_uploader.hpp"
#include <curl/curl.h>
#include <filesystem>
#include "serial_io_libmodbus.hpp"
#include "serial_io_raw.hpp"

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/dist_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

// The Modbus register map of LE-01MR v2 is described in le01mr.hpp

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void onSignal (int) { stopRequested = 1; }

void installSignalHandlers ()
{
	struct sigaction action{};
	action.sa_handler = onSignal;
	sigemptyset (&action.sa_mask);
	action.sa_flags = 0; // no SA_RESTART, blocking calls are interrupted so we quit quickly

	sigaction (SIGINT, &action, nullptr);
	sigaction (SIGTERM, &action, nullptr);
	sigaction (SIGHUP, &action, nullptr); // SSH session closed
}

struct AppConfig {
	SerialSettings serial;
	unsigned slaveId = 1;

	bool rawModeEnabled = false;
	uint8_t rawSkipBytes = 0;
	bool rawSkipZeros = false;

	std::string outputFileName;
	double csvMaxSizeMb = 5.0;
	std::string sftpTarget;
	std::string sftpKey;
	double sftpIntervalMin = 10.0;
	double intervalS = 1.0;
	unsigned timeoutMs = 1000;
	le01mr::ReadMode readMode = le01mr::ReadMode::Auto;
	unsigned maxRegisters = le01mr::DEFAULT_MAX_REGISTERS;
	bool swapWords = false;

	bool noUi = false;
	bool verbose = false;
	bool modbusDebug = false;
	std::string logFile;

	double maxCurrentA = 32.0;
	double maxPowerW = 0.0; //!< 0 - calculated from maxCurrentA
};

[[noreturn]] void exitWithUsage (const boost::program_options::options_description &options,
								 const std::string &error, int code)
{
	if (!error.empty ()) {
		SPDLOG_ERROR ("{}", error);
	}
	std::cout << "Usage: le-01mr --port /dev/ttyUSB0 [options]" << std::endl
			  << options << std::endl;
	exit (code);
}

AppConfig parseArgs (int argc, char *argv[])
{
	namespace po = boost::program_options;

	AppConfig config;
	std::string parity = "N";
	std::string readMode = "auto";
	unsigned rawSkipBytes = 0;

	// ----------------
	// GENERAL OPTIONS
	// ----------------
	po::options_description generalOptions ("General Options");
	generalOptions.add_options () ("help,h", " : Show this help");
	generalOptions.add_options () ("port,P",
								   po::value<std::string> (&config.serial.port),
								   " : Serial port used for communication");
	generalOptions.add_options () ("slave,s",
								   po::value<unsigned> (&config.slaveId)->default_value (1),
								   " : Modbus slave address of the meter");
	generalOptions.add_options () ("baud,b",
								   po::value<int> (&config.serial.baudrate)->default_value (9600),
								   " : Baudrate");
	generalOptions.add_options () ("parity",
								   po::value<std::string> (&parity)->default_value ("N"),
								   " : Parity: N, E or O");
	generalOptions.add_options () ("stop-bits",
								   po::value<int> (&config.serial.stopBits)->default_value (1),
								   " : Number of stop bits: 1 or 2");
	generalOptions.add_options () ("mode-485,4", " : Set to use MODBUS_RTU_RS485 instead of _RS232");
	generalOptions.add_options () ("raw,R", " : Use low level functions for communication");
	generalOptions.add_options () ("out,O",
								   po::value<std::string> (&config.outputFileName),
								   " : Path to output CSV file. Data are appended to an existing "
								   "file. By default le01mr_YYYYmmdd_HHMMSS.csv is created");
	generalOptions.add_options () (
		"csv-max-size",
		po::value<double> (&config.csvMaxSizeMb)->default_value (5.0),
		" : CSV file size in MB which triggers rotation, 0 disables it. An auto-named file is "
		"closed and a new one with a new timestamp is created. A file named with --out is "
		"renamed to <name>_YYYYmmdd_HHMMSS.<ext> and a new file with the same name is created");
	generalOptions.add_options () (
		"interval,i",
		po::value<double> (&config.intervalS)->default_value (1.0),
		" : Interval between readings in seconds");
	generalOptions.add_options () (
		"timeout,t",
		po::value<unsigned> (&config.timeoutMs)->default_value (1000),
		" : Response timeout in milliseconds");
	generalOptions.add_options () (
		"read-mode",
		po::value<std::string> (&readMode)->default_value ("auto"),
		" : How registers are requested: 'block' - as few requests as possible (limited by "
		"--max-registers), spanning over unmapped registers, 'grouped' - one request per "
		"contiguous group of registers (12 requests), 'auto' - try 'block' first and switch to "
		"'grouped' if the meter rejects it");
	generalOptions.add_options () (
		"max-registers",
		po::value<unsigned> (&config.maxRegisters)->default_value (le01mr::DEFAULT_MAX_REGISTERS),
		" : Maximum number of registers read with a single request in 'block' mode, 2..125. "
		"LE-01MR v2 accepts up to 100");
	generalOptions.add_options () ("swap-words",
								   " : 32 bit values are sent low word first (default: high word "
								   "first)");

	// ----------------
	// SFTP OPTIONS
	// ----------------
	po::options_description sftpOptions ("SFTP Upload Options");
	sftpOptions.add_options () (
		"sftp",
		po::value<std::string> (&config.sftpTarget),
		" : Upload the CSV file to this remote directory: [user@]host[:port][/path], e.g. "
		"example.net/directory1/directory2. The path is absolute, use ~/dir for a directory in "
		"the remote home. Existing remote files are overwritten. Authentication with ssh-agent or "
		"keys from ~/.ssh, the host must be in ~/.ssh/known_hosts");
	sftpOptions.add_options () (
		"sftp-interval",
		po::value<double> (&config.sftpIntervalMin)->default_value (10.0),
		" : Interval between uploads in minutes. The file is also uploaded before it is rotated "
		"and when the application exits");
	sftpOptions.add_options () ("sftp-key",
								po::value<std::string> (&config.sftpKey),
								" : Private key file used instead of ssh-agent and default keys");

	// ----------------
	// DISPLAY OPTIONS
	// ----------------
	po::options_description displayOptions ("Display Options");
	displayOptions.add_options () ("no-ui",
								   " : Don't use full screen console UI, print readings line by "
								   "line instead. Used automatically if output isn't a terminal");
	displayOptions.add_options () (
		"max-current",
		po::value<double> (&config.maxCurrentA)->default_value (32.0),
		" : Full scale of the current bar graph [A]");
	displayOptions.add_options () ("max-power",
								   po::value<double> (&config.maxPowerW),
								   " : Full scale of power bar graphs [W]. By default: 230 V * "
								   "max-current");
	displayOptions.add_options () ("verbose,v", " : Show debug messages");
	displayOptions.add_options () ("log-file",
								   po::value<std::string> (&config.logFile),
								   " : Additionally store all messages in this file");
	displayOptions.add_options () ("modbus-debug",
								   " : Enable libmodbus debug output (only with --no-ui)");

	// -----------------
	// RTS OPTIONS
	// -----------------
	po::options_description rtsOptions ("RTS line Options");
	rtsOptions.add_options () (
		"rts-delay",
		po::value<int> (&config.serial.rtsDelayUs),
		" : Delay in microseconds between setting RTS line and starting transmission. By "
		"default: 50,000");
	rtsOptions.add_options () (
		"rts-mode",
		po::value<int> (&config.serial.rtsMode),
		" : State of RTS line in active state: 0 - don't toggle, 1 - RTS_UP, 2 - RTS_DOWN. By "
		"default: RTS_UP");

	// -----------------
	// RAW MODE OPTIONS
	// -----------------
	po::options_description rawOptions ("Raw Mode Options");
	rawOptions.add_options () (
		"raw-skip-zeros",
		" : Skips all preceding zeros before first non-zero byte of a response.");
	rawOptions.add_options () ("raw-skip-bytes",
							   po::value<unsigned> (&rawSkipBytes),
							   " : Skips specified number of bytes from the begining of a response.");

	po::options_description optionsDescription ("");
	optionsDescription.add (generalOptions)
		.add (sftpOptions)
		.add (displayOptions)
		.add (rtsOptions)
		.add (rawOptions);

	// PARSING
	po::variables_map vm;
	try {
		po::store (po::parse_command_line (argc, argv, optionsDescription), vm);
		po::notify (vm);
	}
	catch (const po::error &ex) {
		exitWithUsage (optionsDescription, std::string ("Missing or wrong parameter! ") + ex.what (), -2);
	}

	if (vm.count ("help")) {
		exitWithUsage (optionsDescription, "", 0);
	}

	if (config.serial.port.empty ()) {
		exitWithUsage (optionsDescription, "Serial port not specified!", -3);
	}

	if (config.slaveId < 1 || config.slaveId > 247) {
		exitWithUsage (optionsDescription, "Slave address must be in range 1..247", -2);
	}

	if (parity.size () != 1 || std::string ("NEO").find (std::toupper (parity[0])) == std::string::npos) {
		exitWithUsage (optionsDescription, "Parity must be N, E or O", -2);
	}
	config.serial.parity = static_cast<char> (std::toupper (parity[0]));

	if (config.serial.stopBits != 1 && config.serial.stopBits != 2) {
		exitWithUsage (optionsDescription, "Number of stop bits must be 1 or 2", -2);
	}

	if (config.intervalS < 0.1) {
		exitWithUsage (optionsDescription, "Interval must be at least 0.1 s", -2);
	}

	if (readMode == "auto") {
		config.readMode = le01mr::ReadMode::Auto;
	}
	else if (readMode == "block") {
		config.readMode = le01mr::ReadMode::Block;
	}
	else if (readMode == "grouped") {
		config.readMode = le01mr::ReadMode::Grouped;
	}
	else {
		exitWithUsage (optionsDescription, "Read mode must be one of: auto, block, grouped", -2);
	}

	if (config.sftpIntervalMin < 0.1) {
		exitWithUsage (optionsDescription, "sftp-interval must be at least 0.1 minute", -2);
	}

	if (config.csvMaxSizeMb < 0.0) {
		exitWithUsage (optionsDescription, "csv-max-size can't be negative", -2);
	}

	if (config.maxRegisters < 2 || config.maxRegisters > 125) {
		exitWithUsage (optionsDescription, "max-registers must be in range 2..125", -2);
	}

	if (config.maxCurrentA <= 0.0 || (vm.count ("max-power") && config.maxPowerW <= 0.0)) {
		exitWithUsage (optionsDescription, "Bar graph full scale must be positive", -2);
	}
	if (config.maxPowerW <= 0.0) {
		config.maxPowerW = 230.0 * config.maxCurrentA;
	}

	config.serial.rs485 = vm.count ("mode-485") > 0;
	config.rawModeEnabled = vm.count ("raw") > 0;
	config.swapWords = vm.count ("swap-words") > 0;
	config.noUi = vm.count ("no-ui") > 0;
	config.verbose = vm.count ("verbose") > 0;
	config.modbusDebug = vm.count ("modbus-debug") > 0;

	if (rawSkipBytes > 255) {
		exitWithUsage (optionsDescription, "raw-skip-bytes must be in range 0..255", -2);
	}
	config.rawSkipBytes = static_cast<uint8_t> (rawSkipBytes);

	if (vm.count ("raw-skip-zeros")) {
		if (config.rawSkipBytes > 0) {
			SPDLOG_ERROR ("Conflicting settings! Cannot use raw-skip-zeros and raw-skip-bytes together");
			exit (-4);
		}
		config.rawSkipZeros = true;
	}

	return config;
}

//! One line summary of a reading, used when the full screen UI is not active
void printReading (const le01mr::Reading &reading)
{
	static constexpr std::array<const char *, le01mr::PARAM_COUNT> symbols = {
		"U", "I", "P", "S", "Q", "f", "PF", "E+", "E-", "E", "Eq+", "Eq-", "Eq",
		"Eq1", "Eq2", "Eq3", "Eq4", "Eaux", "Eqaux", "Pdem"};
	static constexpr std::array<size_t, 9> shown = {le01mr::Voltage,
													le01mr::Current,
													le01mr::ActivePower,
													le01mr::ApparentPower,
													le01mr::ReactivePower,
													le01mr::Frequency,
													le01mr::PowerFactor,
													le01mr::ImportedActiveEnergy,
													le01mr::ExportedActiveEnergy};

	std::string line = CsvWriter::formatLocalTime (reading.timestamp);
	for (const size_t i : shown) {
		const auto &def = le01mr::PARAMS[i];
		if (reading.values[i].has_value ()) {
			line += fmt::format ("  {}={:.{}f}{}{}",
								 symbols[i],
								 *reading.values[i],
								 def.decimals,
								 def.unit[0] == '-' ? "" : " ",
								 def.unit[0] == '-' ? "" : def.unit);
		}
		else {
			line += fmt::format ("  {}=---", symbols[i]);
		}
	}

	std::cout << line << std::endl;
}

} // namespace

int main (int argc, char *argv[])
{
	// All log messages go through a distributing sink: to stderr while the full screen UI is not
	// active, to a ring buffer displayed inside the UI and optionally to a log file
	auto ringSink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt> (256);
	ringSink->set_pattern ("%H:%M:%S %L %v");
	auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt> ();
	consoleSink->set_pattern ("[%H:%M:%S.%e] [%^%l%$] %v");
	auto distSink = std::make_shared<spdlog::sinks::dist_sink_mt> ();
	distSink->add_sink (consoleSink);
	distSink->add_sink (ringSink);
	auto logger = std::make_shared<spdlog::logger> ("le01mr", distSink);
	spdlog::set_default_logger (logger);
	spdlog::set_level (spdlog::level::info);

	const AppConfig config = parseArgs (argc, argv);

	if (config.verbose) {
		spdlog::set_level (spdlog::level::debug);
	}

	if (!config.logFile.empty ()) {
		try {
			auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt> (config.logFile);
			fileSink->set_pattern ("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
			distSink->add_sink (fileSink);
			logger->flush_on (spdlog::level::warn);
		}
		catch (const spdlog::spdlog_ex &ex) {
			SPDLOG_ERROR ("Cannot open log file: {}", ex.what ());
			return -1;
		}
	}

	SPDLOG_INFO ("============ LE-01MR v2 energy meter monitor =============");

	const bool modbusDebug = config.modbusDebug && config.noUi;
	if (config.modbusDebug && !config.noUi) {
		SPDLOG_WARN ("--modbus-debug ignored, it works only together with --no-ui");
	}

	std::unique_ptr<SerialIoIf> io;
	try {
		if (config.rawModeEnabled) {
			SPDLOG_INFO ("RTU communication will be done using raw functions.");
			io = std::make_unique<SerialIoRaw> (config.serial,
												config.rawSkipBytes,
												config.rawSkipZeros,
												std::chrono::milliseconds (config.timeoutMs));
		}
		else {
			SPDLOG_DEBUG ("modbusSetRtsDelay: {}", config.serial.rtsDelayUs);
			SPDLOG_DEBUG ("modbusSetRtuMode: {}", config.serial.rtsMode);
			io = std::make_unique<SerialIoLibmodbus> (config.serial,
													  std::chrono::milliseconds (config.timeoutMs),
													  modbusDebug);
		}
	}
	catch (const std::exception &ex) {
		SPDLOG_ERROR ("{}", ex.what ());
		return -1;
	}

	// open CSV output file for appending, a default name is generated from current date and time
	std::optional<CsvWriter> csv;
	try {
		csv.emplace (config.outputFileName,
					 static_cast<std::uintmax_t> (config.csvMaxSizeMb * 1024.0 * 1024.0));
	}
	catch (const std::exception &ex) {
		SPDLOG_ERROR ("{}", ex.what ());
		return -1;
	}
	SPDLOG_INFO ("Readings are stored in: {}", csv->path ());

	// optional upload of the CSV file to a remote server
	std::unique_ptr<SftpUploader> uploader;
	const auto sftpInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
		std::chrono::duration<double, std::ratio<60>> (config.sftpIntervalMin));
	auto nextUpload = std::chrono::steady_clock::now () + sftpInterval;

	if (!config.sftpTarget.empty ()) {
		curl_global_init (CURL_GLOBAL_DEFAULT);
		try {
			uploader = std::make_unique<SftpUploader> (config.sftpTarget, config.sftpKey);
		}
		catch (const std::exception &ex) {
			SPDLOG_ERROR ("{}", ex.what ());
			return -1;
		}

		// the last version of a file is uploaded before it is closed by the rotation
		csv->setRotationCallback ([&uploader] (const std::string &path,
											   std::uintmax_t size,
											   const std::string &finalName) {
			uploader->upload (path, size, finalName, true);
		});
	}

	le01mr::Meter meter{*io,
						static_cast<uint8_t> (config.slaveId),
						config.readMode,
						config.swapWords,
						static_cast<uint16_t> (config.maxRegisters)};

	// handlers must be installed before the UI, so ncurses won't install its own
	installSignalHandlers ();

	std::unique_ptr<ConsoleUi> ui;
	if (!config.noUi) {
		UiConfig uiConfig;
		uiConfig.connection = io->description ();
		uiConfig.slaveId = config.slaveId;
		uiConfig.intervalS = config.intervalS;
		uiConfig.csvPath = csv->path ();
		uiConfig.maxCurrentA = config.maxCurrentA;
		uiConfig.maxPowerW = config.maxPowerW;

		ui = ConsoleUi::create (uiConfig, ringSink);
		if (ui) {
			distSink->remove_sink (consoleSink);
			ui->draw ();
		}
		else {
			SPDLOG_WARN ("Not running on a terminal, full screen UI disabled");
		}
	}

	const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
		std::chrono::duration<double> (config.intervalS));
	auto nextReading = std::chrono::steady_clock::now ();
	unsigned long rowsStored = 0;

	while (stopRequested == 0) {
		nextReading += period;

		// read data from Electrical energy meter LE-01MR v2 and decode them to physical values
		try {
			const le01mr::Reading reading = meter.read ();

			if (ui) {
				ui->onReadSuccess (reading);
			}
			else {
				printReading (reading);
			}

			// store all values in CSV file
			if (csv->write (reading)) {
				rowsStored++;
			}
			else {
				SPDLOG_ERROR ("Writing to {} failed!", csv->path ());
			}

			if (ui) {
				ui->setCsvPath (csv->path ()); // changes when the file is rotated
			}

			if (uploader && std::chrono::steady_clock::now () >= nextUpload) {
				nextUpload = std::chrono::steady_clock::now () + sftpInterval;
				uploader->upload (csv->path (),
								  csv->size (),
								  std::filesystem::path (csv->path ()).filename ().string (),
								  false);
			}
		}
		catch (const std::exception &ex) {
			if (stopRequested != 0) {
				break; // interrupted by a signal, not a real communication problem
			}
			SPDLOG_ERROR ("Reading the meter failed: {}", ex.what ());
			if (ui) {
				ui->onReadFailure (ex.what ());
			}
		}

		// update display and wait for the next reading, while handling key presses. Keyboard is
		// polled at least once per cycle, even if communication took longer than the interval
		while (stopRequested == 0) {
			if (ui) {
				if (uploader) {
					ui->setSftpStatus (uploader->status (), uploader->failing ());
				}
				ui->draw ();
			}

			const auto remainingMs = std::max<long long> (
				0,
				std::chrono::ceil<std::chrono::milliseconds> (nextReading -
															  std::chrono::steady_clock::now ())
					.count ());
			const int waitMs = static_cast<int> (std::min<long long> (remainingMs, 250));

			if (ui) {
				if (!ui->processInput (waitMs)) {
					stopRequested = 1;
				}
			}
			else if (waitMs > 0) {
				std::this_thread::sleep_for (std::chrono::milliseconds (waitMs));
			}

			if (std::chrono::steady_clock::now () >= nextReading) {
				break;
			}
		}

		// communication took longer than the interval, don't try to catch up
		const auto now = std::chrono::steady_clock::now ();
		if (now > nextReading) {
			nextReading = now;
		}
	}

	if (ui) {
		ui.reset ();
		distSink->add_sink (consoleSink);
	}

	if (uploader) {
		SPDLOG_INFO ("SFTP: uploading the final version of {}", csv->path ());
		uploader->upload (csv->path (),
						  csv->size (),
						  std::filesystem::path (csv->path ()).filename ().string (),
						  true);
		if (!uploader->finish (std::chrono::seconds (60))) {
			SPDLOG_ERROR ("SFTP: not all uploads finished in time");
		}
		uploader.reset ();
		curl_global_cleanup ();
	}

	SPDLOG_INFO ("Finished, {} readings stored in {}", rowsStored, csv->path ());

	return 0;
}
