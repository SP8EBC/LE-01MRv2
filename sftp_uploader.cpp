#include "sftp_uploader.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <curl/curl.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdexcept>
#include <unistd.h>

#pragma push_macro("U")
#undef U
// pragma required as a workaround of possible conflict with cpprestsdk.
// more info here: https://github.com/fmtlib/fmt/issues/3330
#include "spdlog/spdlog.h"
#pragma pop_macro("U")

namespace {

//! number of attempts of an important upload
constexpr int MAX_ATTEMPTS = 10;

//! delay between attempts of an important upload
constexpr auto RETRY_DELAY = std::chrono::seconds (60);

std::string timeNow ()
{
	const std::time_t t = std::time (nullptr);
	std::tm local{};
	localtime_r (&t, &local);
	char buffer[16];
	const size_t length = std::strftime (buffer, sizeof (buffer), "%H:%M:%S", &local);
	return std::string (buffer, length);
}

std::string homeDirectory ()
{
	if (const char *home = std::getenv ("HOME"); home != nullptr && *home != '\0') {
		return home;
	}
	if (const passwd *pw = getpwuid (getuid ()); pw != nullptr) {
		return pw->pw_dir;
	}
	return ".";
}

std::string urlEscape (const std::string &text)
{
	char *escaped = curl_easy_escape (nullptr, text.c_str (), static_cast<int> (text.size ()));
	if (escaped == nullptr) {
		throw std::runtime_error ("curl_easy_escape failed");
	}
	std::string result{escaped};
	curl_free (escaped);
	return result;
}

struct ReadState {
	int fd;
	std::uintmax_t offset;
	std::uintmax_t size;
};

size_t readCallback (char *buffer, size_t size, size_t count, void *userdata)
{
	auto *state = static_cast<ReadState *> (userdata);
	const std::uintmax_t wanted =
		std::min<std::uintmax_t> (size * count, state->size - state->offset);
	if (wanted == 0) {
		return 0;
	}

	const ssize_t result = pread (state->fd, buffer, wanted, static_cast<off_t> (state->offset));
	if (result <= 0) {
		// error, or the file is shorter than expected
		return CURL_READFUNC_ABORT;
	}

	state->offset += static_cast<std::uintmax_t> (result);
	return static_cast<size_t> (result);
}

size_t discardCallback (char *, size_t size, size_t count, void *) { return size * count; }

std::string describeError (CURLcode result, const char *errorBuffer)
{
	std::string error = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror (result);
	if (result == CURLE_PEER_FAILED_VERIFICATION) {
		error += " (host key unknown or changed: connect once with ssh to add the server to "
				 "~/.ssh/known_hosts)";
	}
	else if (result == CURLE_LOGIN_DENIED) {
		error += " (public key authentication failed, check ssh-agent, ~/.ssh keys or --sftp-key)";
	}
	return error;
}

} // namespace

struct SftpUploader::FileHandle {
	int fd;
	explicit FileHandle (int fd) : fd{fd} {}
	~FileHandle ()
	{
		if (fd >= 0) {
			::close (fd);
		}
	}
	FileHandle (const FileHandle &) = delete;
	FileHandle &operator= (const FileHandle &) = delete;
};

SftpUploader::SftpUploader (const std::string &target, const std::string &privateKey)
	: m_privateKey{privateKey}, m_knownHosts{homeDirectory () + "/.ssh/known_hosts"}
{
	std::string rest = target;
	if (rest.rfind ("sftp://", 0) == 0) {
		rest = rest.substr (7);
	}

	const size_t slash = rest.find ('/');
	m_hostPart = rest.substr (0, slash);
	const std::string path = slash == std::string::npos ? "" : rest.substr (slash + 1);

	if (m_hostPart.empty () || m_hostPart.back () == '@' || m_hostPart.front () == '@') {
		throw std::invalid_argument ("Invalid SFTP target '" + target +
									 "', expected [user@]host[:port][/path]");
	}

	// URL-encode every path segment, "~" (remote home directory) is kept as it is
	size_t start = 0;
	while (start <= path.size ()) {
		const size_t end = std::min (path.find ('/', start), path.size ());
		const std::string segment = path.substr (start, end - start);
		if (!segment.empty ()) {
			m_remoteDir += "/" + (segment == "~" ? segment : urlEscape (segment));
		}
		start = end + 1;
	}

	// no path given: remote home directory
	if (m_remoteDir.empty ()) {
		m_remoteDir = "/~";
	}

	// don't show a password, if the user put it into the target
	const size_t at = m_hostPart.rfind ('@');
	const size_t colon = m_hostPart.find (':');
	m_displayName = (at != std::string::npos && colon < at)
						? m_hostPart.substr (0, colon) + m_hostPart.substr (at)
						: m_hostPart;
	m_displayName += path.empty () ? "" : "/" + path;

	m_status = "checking connection...";
	m_worker = std::thread (&SftpUploader::run, this);
}

SftpUploader::~SftpUploader ()
{
	{
		std::lock_guard lock{m_mutex};
		m_stop = true;
		m_abort = true;
	}
	m_cv.notify_all ();
	if (m_worker.joinable ()) {
		m_worker.join ();
	}
}

void SftpUploader::upload (const std::string &localPath, std::uintmax_t size,
						   const std::string &remoteName, bool important)
{
	const int fd = ::open (localPath.c_str (), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		SPDLOG_ERROR ("SFTP: cannot open {} for upload", localPath);
		return;
	}

	Job job{std::make_shared<FileHandle> (fd), size, remoteName, important};
	job.notBefore = std::chrono::steady_clock::now ();

	{
		std::lock_guard lock{m_mutex};

		// a waiting upload of the same file is replaced with the newer one
		const auto existing = std::find_if (m_queue.begin (), m_queue.end (), [&] (const Job &j) {
			return !j.important && j.remoteName == remoteName;
		});
		if (existing != m_queue.end ()) {
			*existing = std::move (job);
		}
		else {
			m_queue.push_back (std::move (job));
		}
	}
	m_cv.notify_all ();
}

bool SftpUploader::finish (std::chrono::seconds timeout)
{
	bool done;
	{
		std::unique_lock lock{m_mutex};
		m_stop = true;
		m_cv.notify_all ();
		done = m_cv.wait_for (lock, timeout, [this] { return m_queue.empty () && !m_busy; });
		m_abort = true;
	}
	m_cv.notify_all ();
	if (m_worker.joinable ()) {
		m_worker.join ();
	}
	return done;
}

std::string SftpUploader::status () const
{
	std::lock_guard lock{m_mutex};
	// status first, so it stays visible when a long target doesn't fit on the screen
	return m_status + "   -> " + m_displayName;
}

bool SftpUploader::failing () const
{
	std::lock_guard lock{m_mutex};
	return m_failing;
}

int SftpUploader::progressCallback (void *self, long long, long long, long long, long long)
{
	// non zero return value aborts the transfer
	return static_cast<SftpUploader *> (self)->m_abort ? 1 : 0;
}

void SftpUploader::setupCommon (void *handle, std::string &url, char *errorBuffer) const
{
	CURL *curl = static_cast<CURL *> (handle);

	curl_easy_setopt (curl, CURLOPT_URL, url.c_str ());
	curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt (curl, CURLOPT_SSH_AUTH_TYPES,
					  static_cast<long> (CURLSSH_AUTH_PUBLICKEY | CURLSSH_AUTH_AGENT));
	curl_easy_setopt (curl, CURLOPT_SSH_KNOWNHOSTS, m_knownHosts.c_str ());
	if (!m_privateKey.empty ()) {
		curl_easy_setopt (curl, CURLOPT_SSH_PRIVATE_KEYFILE, m_privateKey.c_str ());
	}
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 20L);

	// abort a stalled transfer
	curl_easy_setopt (curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt (curl, CURLOPT_LOW_SPEED_TIME, 60L);

	// allows aborting a transfer when the application is closing
	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION, &SftpUploader::progressCallback);
	curl_easy_setopt (curl, CURLOPT_XFERINFODATA, this);
}

void SftpUploader::checkConnection ()
{
	CURL *curl = curl_easy_init ();
	if (curl == nullptr) {
		return;
	}

	// URL ending with a slash lists the directory, which checks if it exists and is accessible
	std::string url = "sftp://" + m_hostPart + m_remoteDir + "/";
	char errorBuffer[CURL_ERROR_SIZE] = {0};
	setupCommon (curl, url, errorBuffer);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, &discardCallback);

	const CURLcode result = curl_easy_perform (curl);
	curl_easy_cleanup (curl);

	std::lock_guard lock{m_mutex};
	if (result == CURLE_OK) {
		SPDLOG_INFO ("SFTP: connected to {}, remote directory is accessible", m_displayName);
		m_status = "connection OK, waiting for the first upload";
		m_failing = false;
	}
	else if (!m_abort) {
		const std::string error = describeError (result, errorBuffer);
		SPDLOG_ERROR ("SFTP: connection check of {} failed: {}", m_displayName, error);
		m_status = "connection check FAILED: " + error;
		m_failing = true;
	}
}

void SftpUploader::perform (const Job &job)
{
	CURL *curl = curl_easy_init ();
	if (curl == nullptr) {
		throw std::runtime_error ("curl_easy_init failed");
	}

	std::string url = "sftp://" + m_hostPart + m_remoteDir + "/" + urlEscape (job.remoteName);
	char errorBuffer[CURL_ERROR_SIZE] = {0};
	ReadState state{job.file->fd, 0, job.size};

	setupCommon (curl, url, errorBuffer);
	curl_easy_setopt (curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt (curl, CURLOPT_READFUNCTION, &readCallback);
	curl_easy_setopt (curl, CURLOPT_READDATA, &state);
	curl_easy_setopt (curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t> (job.size));
	curl_easy_setopt (curl, CURLOPT_NEW_FILE_PERMS, 0644L);

	const auto started = std::chrono::steady_clock::now ();
	const CURLcode result = curl_easy_perform (curl);
	curl_easy_cleanup (curl);

	if (result != CURLE_OK) {
		throw std::runtime_error (describeError (result, errorBuffer));
	}

	SPDLOG_INFO ("SFTP: uploaded {} ({} kB) in {:.1f} s",
				 job.remoteName,
				 job.size / 1024,
				 std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count ());
}

void SftpUploader::run ()
{
	checkConnection ();

	std::unique_lock lock{m_mutex};

	while (true) {
		if (m_abort || (m_stop && m_queue.empty ())) {
			break;
		}

		if (m_queue.empty ()) {
			m_cv.wait (lock);
			continue;
		}

		// the first job which may be started now. When finishing, retry delays are ignored
		const auto now = std::chrono::steady_clock::now ();
		auto ready = std::find_if (m_queue.begin (), m_queue.end (), [&] (const Job &j) {
			return m_stop || j.notBefore <= now;
		});
		if (ready == m_queue.end ()) {
			const auto earliest = std::min_element (m_queue.begin (),
													m_queue.end (),
													[] (const Job &a, const Job &b) {
														return a.notBefore < b.notBefore;
													});
			m_cv.wait_until (lock, earliest->notBefore);
			continue;
		}

		Job job = std::move (*ready);
		m_queue.erase (ready);
		m_busy = true;
		lock.unlock ();

		std::string error;
		try {
			perform (job);
		}
		catch (const std::exception &ex) {
			error = ex.what ();
		}

		lock.lock ();
		m_busy = false;

		if (error.empty ()) {
			m_status = "last upload " + timeNow () + " OK (" + job.remoteName + ")";
			m_failing = false;
		}
		else if (!m_abort) {
			m_status = "upload FAILED " + timeNow () + " (" + job.remoteName + "): " + error;
			m_failing = true;

			job.attempts++;
			if (job.important && job.attempts < MAX_ATTEMPTS && !m_stop) {
				SPDLOG_ERROR ("SFTP: upload of {} failed: {}. Retrying in {} s",
							  job.remoteName,
							  error,
							  RETRY_DELAY.count ());
				job.notBefore = std::chrono::steady_clock::now () + RETRY_DELAY;
				m_queue.push_back (std::move (job));
			}
			else {
				SPDLOG_ERROR ("SFTP: upload of {} failed: {}", job.remoteName, error);
			}
		}

		m_cv.notify_all ();
	}

	m_cv.notify_all ();
}
