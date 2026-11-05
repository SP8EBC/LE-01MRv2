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

//! Returns path, or path with "_1", "_2"... added before the extension if it already exists
std::filesystem::path uniquePath (const std::filesystem::path &path)
{
	std::error_code ec;
	if (!std::filesystem::exists (path, ec)) {
		return path;
	}

	for (int i = 1;; i++) {
		std::filesystem::path candidate = path;
		candidate.replace_filename (path.stem ().string () + "_" + std::to_string (i) +
									path.extension ().string ());
		if (!std::filesystem::exists (candidate, ec)) {
			return candidate;
		}
	}
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

CsvWriter::CsvWriter (const std::string &path, std::uintmax_t maxSizeBytes)
	: m_autoName{path.empty ()}, m_maxSize{maxSizeBytes},
	  m_path{path.empty () ? uniquePath (defaultFileName ()).string () : path}
{
	open ();
}

void CsvWriter::open ()
{
	std::error_code ec;
	m_size = std::filesystem::exists (m_path, ec) ? std::filesystem::file_size (m_path, ec) : 0;
	if (ec) {
		m_size = 0;
	}

	m_file.open (m_path, std::ios::out | std::ios::app);
	if (!m_file.is_open ()) {
		throw std::runtime_error ("Cannot open CSV output file: " + m_path);
	}

	if (m_size == 0) {
		std::string header = "Timestamp,Unix time";
		for (const auto &param : le01mr::PARAMS) {
			header += fmt::format (",{} [{}]", param.name, param.unit);
		}
		header += '\n';

		m_file << header;
		m_file.flush ();
		m_size += header.size ();
	}

	m_rotateAt = m_maxSize;
}

void CsvWriter::rotate ()
{
	const std::string oldPath = m_path;
	const std::uintmax_t oldSize = m_size;

	// a file named by the user is renamed to <name>_YYYYmmdd_HHMMSS.<ext>
	const std::filesystem::path current{m_path};
	std::filesystem::path archive = current;
	if (!m_autoName) {
		archive.replace_filename (current.stem ().string () +
								  formatTime (std::chrono::system_clock::now (), "_%Y%m%d_%H%M%S") +
								  current.extension ().string ());
		archive = uniquePath (archive);
	}

	if (m_rotationCallback) {
		m_rotationCallback (m_path, m_size, archive.filename ().string ());
	}

	m_file.close ();

	try {
		if (m_autoName) {
			m_path = uniquePath (defaultFileName ()).string ();
			open ();
			SPDLOG_INFO ("CSV file {} reached {} kB, continuing in {}", oldPath, oldSize / 1024, m_path);
		}
		else {
			std::filesystem::rename (current, archive);
			open ();
			SPDLOG_INFO ("CSV file {} reached {} kB, old data moved to {}",
						 m_path,
						 oldSize / 1024,
						 archive.string ());
		}
	}
	catch (const std::exception &ex) {
		SPDLOG_ERROR ("Rotating CSV file {} failed: {}", oldPath, ex.what ());

		// keep writing to the old file and try again after it grows another 10%
		m_path = oldPath;
		if (!m_file.is_open ()) {
			open ();
		}
		m_rotateAt = m_size + m_maxSize / 10;
	}
}

bool CsvWriter::write (const le01mr::Reading &reading)
{
	if (m_maxSize > 0 && m_size >= m_rotateAt) {
		try {
			rotate ();
		}
		catch (const std::exception &ex) {
			// even reopening the old file failed
			SPDLOG_ERROR ("{}", ex.what ());
			return false;
		}
	}

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

	m_size += line.size ();
	return true;
}
