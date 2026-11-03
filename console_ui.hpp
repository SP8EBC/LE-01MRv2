#ifndef D9A6E2B4_7C1F_4E83_A2D5_8B3F6C0E1D72
#define D9A6E2B4_7C1F_4E83_A2D5_8B3F6C0E1D72

#include "le01mr.hpp"
#include <memory>
#include <optional>
#include <string>

#pragma push_macro("U")
#undef U
#include "spdlog/sinks/ringbuffer_sink.h"
#pragma pop_macro("U")

struct UiConfig {
	std::string connection; //!< serial port / backend description
	unsigned slaveId = 1;
	double intervalS = 1.0;
	std::string csvPath;
	double maxCurrentA = 32.0; //!< full scale of the current bar
	double maxPowerW = 7360.0; //!< full scale of power bars
};

/**
 * htop-like full screen console UI, built on ncurses. Works on any terminal, also via SSH.
 */
class ConsoleUi {
  public:
	/**
	 * @return nullptr if stdin/stdout isn't a terminal or the terminal can't be initialized
	 */
	static std::unique_ptr<ConsoleUi>
	create (UiConfig config, std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logSink);

	~ConsoleUi ();

	ConsoleUi (const ConsoleUi &) = delete;
	ConsoleUi &operator= (const ConsoleUi &) = delete;

	void onReadSuccess (const le01mr::Reading &reading);
	void onReadFailure (const std::string &error);

	void draw ();

	/**
	 * Waits up to timeoutMs for a key press and handles it (including terminal resize).
	 * @return false if the user requested to quit
	 */
	bool processInput (int timeoutMs);

  private:
	ConsoleUi (UiConfig config, std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logSink,
			   void *screen);

	void put (int y, int x, const std::string &text, int attributes = 0);
	int drawHeader (int y);
	int drawBars (int y);
	int drawCounters (int y);
	void drawLog (int y, int lastRow);
	void drawFooter ();

	UiConfig m_config;
	std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> m_logSink;
	void *m_screen; //!< SCREEN*, kept opaque so curses.h macros don't leak into other units
	bool m_colors = false;
	bool m_compact = false;		  //!< no separators and headings, for small terminals
	bool m_drawingFooter = false; //!< allows drawing in the last row
	int m_rows = 0;
	int m_cols = 0;

	std::optional<le01mr::Reading> m_lastReading;
	bool m_lastReadFailed = false;
	std::string m_lastError;
	std::chrono::system_clock::time_point m_lastErrorTime;
	unsigned long m_readsOk = 0;
	unsigned long m_readsFailed = 0;
};

#endif /* D9A6E2B4_7C1F_4E83_A2D5_8B3F6C0E1D72 */
