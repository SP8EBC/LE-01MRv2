#ifndef F2C8A4E6_9D3B_4A71_B5E2_0C6D8F1A7B39
#define F2C8A4E6_9D3B_4A71_B5E2_0C6D8F1A7B39

#include "le01mr.hpp"
#include <fstream>
#include <string>

/**
 * Appends readings to a CSV file. A header line is written when the file is new or empty.
 * Every row is flushed immediately, so no data is lost when the application is killed.
 */
class CsvWriter {
  public:
	//! @throws std::runtime_error if the file cannot be opened
	explicit CsvWriter (std::string path);

	//! @return false if writing failed (e.g. disk full)
	bool write (const le01mr::Reading &reading);

	const std::string &path () const { return m_path; }

	//! le01mr_YYYYmmdd_HHMMSS.csv generated from current local date and time
	static std::string defaultFileName ();

	//! YYYY-mm-dd HH:MM:SS in local time
	static std::string formatLocalTime (std::chrono::system_clock::time_point time);

  private:
	std::string m_path;
	std::ofstream m_file;
};

#endif /* F2C8A4E6_9D3B_4A71_B5E2_0C6D8F1A7B39 */
