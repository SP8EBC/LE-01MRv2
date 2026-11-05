#include "console_ui.hpp"
#include "csv_writer.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>
#include <unistd.h>

#pragma push_macro("U")
#undef U
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

// curses.h has to be included last. Without NCURSES_NOMACROS it defines function-like macros
// such as move(), erase() or clear() which break the standard library
#define NCURSES_NOMACROS
#include <curses.h>

namespace {

enum ColorPair : short {
	CP_GREEN = 1,
	CP_YELLOW,
	CP_RED,
	CP_CYAN,
	CP_TITLE,
	CP_LABEL,
	CP_FOOTER_KEY,
};

//! width of a label on the left side of a bar graph
constexpr int BAR_LABEL_W = 16;

//! width of a value + unit on the right side of a bar graph, e.g. "    230.123 V    "
constexpr int VALUE_W = 18;

//! width of a label of an energy counter
constexpr int COUNTER_LABEL_W = 35;

//! width of one column with energy counters
constexpr int COUNTER_COL_W = COUNTER_LABEL_W + VALUE_W;

struct BarSpec {
	enum class Style {
		Load,	 //!< green -> yellow -> red as the bar fills, cyan if negative (export)
		Band,	 //!< green inside <bandLow, bandHigh>, red outside
		Quality, //!< green if high, red if low
	};

	double min;
	double max;
	bool absolute; //!< the bar shows the absolute value, the sign is shown by the color
	Style style;
	double bandLow = 0.0;
	double bandHigh = 0.0;
};

int colorAttr (bool colors, short pair, int monochromeFallback = A_NORMAL)
{
	return colors ? static_cast<int> (COLOR_PAIR (pair)) : monochromeFallback;
}

std::string formatValue (const std::optional<double> &value, const le01mr::ParamDef &def)
{
	const char *unit = std::string_view (def.unit) == "-" ? "" : def.unit;
	if (!value.has_value ()) {
		return fmt::format ("{:>11} {:<5}", "---", unit);
	}
	return fmt::format ("{:>11.{}f} {:<5}", *value, def.decimals, unit);
}

} // namespace

std::unique_ptr<ConsoleUi>
ConsoleUi::create (UiConfig config, std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logSink)
{
	if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO)) {
		return nullptr;
	}

	SCREEN *screen = newterm (nullptr, stdout, stdin);
	if (screen == nullptr) {
		return nullptr;
	}

	return std::unique_ptr<ConsoleUi> (
		new ConsoleUi (std::move (config), std::move (logSink), screen));
}

ConsoleUi::ConsoleUi (UiConfig config, std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logSink,
					  void *screen)
	: m_config{std::move (config)}, m_logSink{std::move (logSink)}, m_screen{screen}
{
	set_term (static_cast<SCREEN *> (m_screen));
	cbreak ();
	noecho ();
	keypad (stdscr, TRUE);
	curs_set (0);

	if (has_colors ()) {
		m_colors = true;
		start_color ();
		use_default_colors ();
		init_pair (CP_GREEN, COLOR_GREEN, -1);
		init_pair (CP_YELLOW, COLOR_YELLOW, -1);
		init_pair (CP_RED, COLOR_RED, -1);
		init_pair (CP_CYAN, COLOR_CYAN, -1);
		init_pair (CP_TITLE, COLOR_BLACK, COLOR_GREEN);
		init_pair (CP_LABEL, COLOR_CYAN, -1);
		init_pair (CP_FOOTER_KEY, COLOR_BLACK, COLOR_CYAN);
	}
}

ConsoleUi::~ConsoleUi ()
{
	endwin ();
	delscreen (static_cast<SCREEN *> (m_screen));
}

void ConsoleUi::onReadSuccess (const le01mr::Reading &reading)
{
	m_lastReading = reading;
	m_lastReadFailed = false;
	m_readsOk++;
}

void ConsoleUi::onReadFailure (const std::string &error)
{
	m_lastReadFailed = true;
	m_lastError = error;
	m_lastErrorTime = std::chrono::system_clock::now ();
	m_readsFailed++;
}

void ConsoleUi::put (int y, int x, const std::string &text, int attributes)
{
	// the last row is reserved for the footer
	const int lastRow = m_drawingFooter ? m_rows : m_rows - 1;
	if (y < 0 || y >= lastRow || x >= m_cols) {
		return;
	}
	const int length = std::min<int> (static_cast<int> (text.size ()), m_cols - x);
	if (length <= 0) {
		return;
	}
	attron (attributes);
	mvaddnstr (y, x, text.c_str (), length);
	attroff (attributes);
}

int ConsoleUi::drawHeader (int y)
{
	const int titleAttr = colorAttr (m_colors, CP_TITLE, A_REVERSE) | A_BOLD;
	const std::string now = CsvWriter::formatLocalTime (std::chrono::system_clock::now ());

	put (y, 0, std::string (m_cols, ' '), titleAttr);
	put (y, 1, "LE-01MR v2 energy meter monitor", titleAttr);
	put (y, std::max (0, m_cols - static_cast<int> (now.size ()) - 1), now, titleAttr);
	y++;

	const int labelAttr = colorAttr (m_colors, CP_LABEL);

	const std::string connection = fmt::format ("{}   slave: {}   interval: {:.1f} s",
												m_config.connection,
												m_config.slaveId,
												m_config.intervalS);

	put (y, 1, "Port:", labelAttr);
	put (y, 7, connection);

	if (m_compact) {
		// both on one line, to save a row on small terminals
		const int x = 7 + static_cast<int> (connection.size ()) + 3;
		put (y, x, "CSV:", labelAttr);
		put (y, x + 5, m_config.csvPath);
	}
	else {
		y++;
		put (y, 1, "CSV: ", labelAttr);
		put (y, 7, m_config.csvPath);

		if (!m_sftpStatus.empty ()) {
			y++;
			put (y, 1, "SFTP:", labelAttr);
			put (y,
				 7,
				 m_sftpStatus,
				 m_sftpFailing ? colorAttr (m_colors, CP_RED, A_BOLD) : static_cast<int> (A_NORMAL));
		}
	}
	y++;

	put (y, 1, "Status:", labelAttr);
	int x = 9;
	std::string status;
	int statusAttr;
	if (m_readsOk == 0 && m_readsFailed == 0) {
		status = "connecting...";
		statusAttr = colorAttr (m_colors, CP_YELLOW, A_BOLD);
	}
	else if (m_lastReadFailed) {
		status = "NO RESPONSE";
		statusAttr = colorAttr (m_colors, CP_RED, A_BOLD) | A_BOLD;
	}
	else {
		status = "OK";
		statusAttr = colorAttr (m_colors, CP_GREEN) | A_BOLD;
	}
	put (y, x, status, statusAttr);
	x += static_cast<int> (status.size ()) + 3;

	std::string details;
	if (m_lastReading.has_value ()) {
		details += "last reading: " + CsvWriter::formatLocalTime (m_lastReading->timestamp) + "   ";
	}
	details += fmt::format ("reads: {} ok, {} failed", m_readsOk, m_readsFailed);
	if (m_compact && m_lastReadFailed) {
		details += "   " + m_lastError;
	}
	if (m_compact && m_sftpFailing) {
		details += "   SFTP upload FAILED";
	}
	put (y, x, details);
	y++;

	if (m_compact) {
		return y;
	}

	if (!m_lastError.empty ()) {
		const int errorAttr =
			m_lastReadFailed ? colorAttr (m_colors, CP_RED, A_BOLD) : static_cast<int> (A_DIM);
		put (y,
			 1,
			 fmt::format ("Last error ({}): {}",
						  CsvWriter::formatLocalTime (m_lastErrorTime),
						  m_lastError),
			 errorAttr);
	}
	y++;

	return y;
}

int ConsoleUi::drawBars (int y)
{
	using Style = BarSpec::Style;

	const double maxI = m_config.maxCurrentA;
	const double maxP = m_config.maxPowerW;

	// indexes follow le01mr::Param, from Voltage to PowerFactor
	const std::array<BarSpec, 7> specs = {{
		{200.0, 260.0, false, Style::Band, 207.0, 253.0}, // EN 50160: 230 V +/- 10%
		{0.0, maxI, true, Style::Load},
		{0.0, maxP, true, Style::Load},
		{0.0, maxP, true, Style::Load},
		{0.0, maxP, true, Style::Load},
		{49.0, 51.0, false, Style::Band, 49.5, 50.5}, // EN 50160: 50 Hz +/- 1%
		{0.0, 1.0, true, Style::Quality},
	}};

	const int barX = 1 + BAR_LABEL_W;
	const int barInner = m_cols - barX - 2 - VALUE_W - 1;
	const int labelAttr = colorAttr (m_colors, CP_LABEL) | A_BOLD;
	const int valueAttr = m_lastReadFailed ? static_cast<int> (A_DIM) : static_cast<int> (A_BOLD);

	for (size_t i = 0; i < specs.size (); i++) {
		const le01mr::ParamDef &def = le01mr::PARAMS[i];
		const BarSpec &spec = specs[i];
		const std::optional<double> value =
			m_lastReading.has_value () ? m_lastReading->values[i] : std::nullopt;

		put (y, 1, def.label, labelAttr);

		if (barInner >= 4) {
			int filled = 0;
			int attr = colorAttr (m_colors, CP_GREEN);

			if (value.has_value ()) {
				const double v = *value;
				const double shown = spec.absolute ? std::fabs (v) : v;
				const double fraction =
					std::clamp ((shown - spec.min) / (spec.max - spec.min), 0.0, 1.0);
				filled = static_cast<int> (std::lround (fraction * barInner));

				switch (spec.style) {
				case Style::Load:
					if (v < 0.0) {
						attr = colorAttr (m_colors, CP_CYAN);
					}
					else if (fraction >= 0.85) {
						attr = colorAttr (m_colors, CP_RED);
					}
					else if (fraction >= 0.6) {
						attr = colorAttr (m_colors, CP_YELLOW);
					}
					break;
				case Style::Band:
					if (shown < spec.bandLow || shown > spec.bandHigh) {
						attr = colorAttr (m_colors, CP_RED);
					}
					break;
				case Style::Quality:
					if (shown < 0.7) {
						attr = colorAttr (m_colors, CP_RED);
					}
					else if (shown < 0.9) {
						attr = colorAttr (m_colors, CP_YELLOW);
					}
					break;
				}
			}

			if (m_lastReadFailed) {
				attr |= A_DIM;
			}

			put (y, barX, "[", A_BOLD);
			put (y, barX + 1, std::string (filled, '|'), attr | A_BOLD);
			put (y, barX + 1 + barInner, "]", A_BOLD);
		}

		put (y, m_cols - VALUE_W - 1, formatValue (value, def), valueAttr);
		y++;
	}

	return m_compact ? y : y + 1;
}

int ConsoleUi::drawCounters (int y)
{
	constexpr size_t first = le01mr::ImportedActiveEnergy;
	constexpr size_t total = le01mr::PARAM_COUNT - first;

	const int columns = m_cols >= 2 * COUNTER_COL_W + 4 ? 2 : 1;
	const size_t perColumn = (total + columns - 1) / columns;
	const int labelAttr = colorAttr (m_colors, CP_LABEL);
	const int valueAttr = m_lastReadFailed ? static_cast<int> (A_DIM) : static_cast<int> (A_BOLD);

	if (!m_compact) {
		put (y++, 1, "Energy counters", A_BOLD | A_UNDERLINE);
	}

	for (size_t n = 0; n < total; n++) {
		const size_t i = first + n;
		const int column = static_cast<int> (n / perColumn);
		const int row = static_cast<int> (n % perColumn);
		const int x = 1 + column * (COUNTER_COL_W + 4);

		const le01mr::ParamDef &def = le01mr::PARAMS[i];
		const std::optional<double> value =
			m_lastReading.has_value () ? m_lastReading->values[i] : std::nullopt;

		std::string label = def.label;
		label += ' ';
		label.resize (COUNTER_LABEL_W, '.');

		put (y + row, x, label, labelAttr);
		put (y + row, x + COUNTER_LABEL_W, formatValue (value, def), valueAttr);
	}

	return y + static_cast<int> (perColumn) + (m_compact ? 0 : 1);
}

void ConsoleUi::drawLog (int y, int lastRow)
{
	const int lines = lastRow - y;
	if (lines < 2 || !m_logSink) {
		return;
	}

	put (y++, 1, "Messages", A_BOLD | A_UNDERLINE);

	const std::vector<std::string> messages = m_logSink->last_formatted (lines - 1);
	for (std::string message : messages) {
		while (!message.empty () && (message.back () == '\n' || message.back () == '\r')) {
			message.pop_back ();
		}

		// pattern is "HH:MM:SS L message", L is a short level name
		int attr = A_NORMAL;
		const char level = message.size () > 9 ? message[9] : 'I';
		if (level == 'E' || level == 'C') {
			attr = colorAttr (m_colors, CP_RED, A_BOLD);
		}
		else if (level == 'W') {
			attr = colorAttr (m_colors, CP_YELLOW);
		}
		else if (level == 'D' || level == 'T') {
			attr = A_DIM;
		}

		put (y++, 1, message, attr);
	}
}

void ConsoleUi::drawFooter ()
{
	const int y = m_rows - 1;
	m_drawingFooter = true;
	const int keyAttr = colorAttr (m_colors, CP_FOOTER_KEY, A_REVERSE);

	put (y, 0, "q", keyAttr);
	put (y, 1, "Quit  ");
	put (y, 7, "^L", keyAttr);
	put (y, 9, "Redraw");
	m_drawingFooter = false;
}

void ConsoleUi::draw ()
{
	m_rows = getmaxy (stdscr);
	m_cols = getmaxx (stdscr);

	// all sections with separators and headings need this many rows, including 3 log lines
	constexpr int FULL_LAYOUT_ROWS = 1 + 4 + 1 + 8 + 1 + 13 + 1 + 4 + 1;
	const int counterRows = m_cols >= 2 * COUNTER_COL_W + 4 ? 7 : 13;
	const int sftpRows = m_sftpStatus.empty () ? 0 : 1;
	m_compact = m_rows < FULL_LAYOUT_ROWS - 13 + counterRows + sftpRows;

	werase (stdscr);

	int y = 0;
	y = drawHeader (y);
	y = drawBars (y);
	y = drawCounters (y);
	drawLog (y, m_rows - 1);
	drawFooter ();

	wrefresh (stdscr);
}

bool ConsoleUi::processInput (int timeoutMs)
{
	wtimeout (stdscr, std::max (0, timeoutMs));
	const int key = wgetch (stdscr);

	switch (key) {
	case 'q':
	case 'Q':
	case KEY_F (10): return false;
	case 12: // Ctrl+L
		clearok (curscr, TRUE);
		draw ();
		break;
	case KEY_RESIZE: draw (); break;
	default: break;
	}

	return true;
}
