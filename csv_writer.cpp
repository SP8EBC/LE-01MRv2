#include "csv_writer.hpp"
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

namespace {

std::string formatTime (std::chrono::system_clock::time_point time, const char *format)
{
	const std::time_t t = std::chrono::system_clock::to_time_t (time);
	std::tm local{};
	localtime_r (&t, &local);

	char buffer[64];
	const size_t length = std::strftime (buffer, sizeof (buffer), format, &local);
	return std::string (buffer, length);
}

} // namespace

std::string CsvWriter::defaultFileName ()
{
	return formatTime (std::chrono::system_clock::now (), "le01mr_%Y%m%d_%H%M%S.csv");
}

std::string CsvWriter::formatLocalTime (std::chrono::system_clock::time_point time)
{
	return formatTime (time, "%Y-%m-%d %H:%M:%S");
}

CsvWriter::CsvWriter (std::string path) : m_path{std::move (path)}
{
	std::error_code ec;
	const bool needsHeader =
		!std::filesystem::exists (m_path, ec) || std::filesystem::file_size (m_path, ec) == 0;

	m_file.open (m_path, std::ios::out | std::ios::app);
	if (!m_file.is_open ()) {
		throw std::runtime_error ("Cannot open CSV output file: " + m_path);
	}

	if (needsHeader) {
		m_file << "Timestamp,Unix time";
		for (const auto &param : le01mr::PARAMS) {
			m_file << ',' << param.name << " [" << param.unit << ']';
		}
		m_file << '\n';
		m_file.flush ();
	}
}

bool CsvWriter::write (const le01mr::Reading &reading)
{
	std::string line = formatLocalTime (reading.timestamp);
	line += fmt::format (
		",{}",
		std::chrono::duration_cast<std::chrono::seconds> (reading.timestamp.time_since_epoch ())
			.count ());

	for (size_t i = 0; i < le01mr::PARAM_COUNT; i++) {
		line += ',';
		if (reading.values[i].has_value ()) {
			// fmt is locale independent, always uses '.' as a decimal separator
			line += fmt::format ("{:.{}f}", *reading.values[i], le01mr::PARAMS[i].decimals);
		}
	}
	line += '\n';

	m_file << line;
	m_file.flush ();

	if (!m_file.good ()) {
		m_file.clear ();
		return false;
	}
	return true;
}
