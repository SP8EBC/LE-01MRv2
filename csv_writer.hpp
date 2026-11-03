#ifndef F2C8A4E6_9D3B_4A71_B5E2_0C6D8F1A7B39
#define F2C8A4E6_9D3B_4A71_B5E2_0C6D8F1A7B39

#include "le01mr.hpp"
#include <cstdint>
#include <fstream>
#include <string>

/**
 * Appends readings to a CSV file. A header line is written when the file is new or empty.
 * Every row is flushed immediately, so no data is lost when the application is killed.
 *
 * When the file grows to maxSizeBytes it is rotated:
 *  - auto-generated name: the file is closed and a new one, with a new timestamp in the name,
 *    is created,
 *  - name given by the user: the file is closed, renamed to "<name>_YYYYmmdd_HHMMSS.<ext>" (time of
 *    rotation) and a new file with the original name is created.
 */
class CsvWriter {
  public:
	static constexpr std::uintmax_t DEFAULT_MAX_SIZE = 5u * 1024u * 1024u;

	/**
	 * @param path			file to write, or empty to generate a name from current date and time
	 * @param maxSizeBytes	size triggering the rotation, 0 disables rotation
	 * @throws std::runtime_error if the file cannot be opened
	 */
	explicit CsvWriter (const std::string &path, std::uintmax_t maxSizeBytes = DEFAULT_MAX_SIZE);

	//! @return false if writing failed (e.g. disk full)
	bool write (const le01mr::Reading &reading);

	//! path of the file currently written
	const std::string &path () const { return m_path; }

	//! le01mr_YYYYmmdd_HHMMSS.csv generated from current local date and time
	static std::string defaultFileName ();

	//! YYYY-mm-dd HH:MM:SS in local time
	static std::string formatLocalTime (std::chrono::system_clock::time_point time);

  private:
	void open ();
	void rotate ();

	const bool m_autoName;
	const std::uintmax_t m_maxSize;
	std::string m_path;
	std::ofstream m_file;
	std::uintmax_t m_size = 0;

	//! size at which the next rotation is attempted, moved forward if a rotation fails
	std::uintmax_t m_rotateAt = 0;
};

#endif /* F2C8A4E6_9D3B_4A71_B5E2_0C6D8F1A7B39 */
