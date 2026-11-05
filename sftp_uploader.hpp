#ifndef B8E4D1A3_6F2C_4D97_A1E5_2C7B9F0D3E61
#define B8E4D1A3_6F2C_4D97_A1E5_2C7B9F0D3E61

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/**
 * Uploads files to a remote directory over SFTP (libcurl), in a background thread, so
 * a slow or unreachable server never blocks reading the meter.
 *
 * Target format: [sftp://][user@]host[:port][/path]
 *  - the path is absolute, like in sftp:// URLs: example.net/data/le01mr -> /data/le01mr
 *  - "~" means the remote home directory: example.net/~/le01mr
 *  - no path: the remote home directory
 *
 * Authentication: public key only (ssh-agent or default keys in ~/.ssh, or a given key file).
 * The server's host key must be present in ~/.ssh/known_hosts.
 * An existing remote file with the same name is overwritten.
 */
class SftpUploader {
  public:
	/**
	 * @param target		remote directory, see the class description
	 * @param privateKey	private key file, empty to use ssh-agent and default keys
	 * @throws std::invalid_argument if the target is malformed
	 */
	SftpUploader (const std::string &target, const std::string &privateKey);

	//! Aborts any transfer in progress and stops the worker thread
	~SftpUploader ();

	SftpUploader (const SftpUploader &) = delete;
	SftpUploader &operator= (const SftpUploader &) = delete;

	/**
	 * Queues upload of the first `size` bytes of a local file as `remoteName`. The local file is
	 * opened immediately, so it may be renamed or closed by the caller right after this call.
	 *
	 * @param important	retried for some time if it fails (e.g. the last upload of a rotated file).
	 *					Not important uploads are only replaced by newer ones with the same name.
	 */
	void upload (const std::string &localPath, std::uintmax_t size, const std::string &remoteName,
				 bool important);

	/**
	 * Waits until all queued uploads are done (or definitely failed) or the timeout expires,
	 * then stops the worker thread.
	 * @return true if everything was uploaded before the timeout
	 */
	bool finish (std::chrono::seconds timeout);

	//! Short status for the UI, e.g. "last upload 20:10:01 OK (file.csv)   -> example.net/data"
	std::string status () const;

	//! Did the last upload (or the connection check) fail?
	bool failing () const;

  private:
	struct FileHandle;

	struct Job {
		std::shared_ptr<FileHandle> file;
		std::uintmax_t size;
		std::string remoteName;
		bool important;
		int attempts = 0;
		std::chrono::steady_clock::time_point notBefore;
	};

	void run ();
	void checkConnection ();
	void perform (const Job &job);
	void setupCommon (void *curl, std::string &url, char *errorBuffer) const;

	static int progressCallback (void *self, long long, long long, long long, long long);

	std::string m_hostPart;	   //!< [user@]host[:port]
	std::string m_remoteDir;   //!< URL-encoded absolute path, without a trailing slash
	std::string m_displayName; //!< target as shown to the user
	std::string m_privateKey;
	std::string m_knownHosts;

	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<Job> m_queue;
	bool m_busy = false;
	bool m_stop = false;
	std::atomic<bool> m_abort{false};

	std::string m_status;
	bool m_failing = false;

	std::thread m_worker;
};

#endif /* B8E4D1A3_6F2C_4D97_A1E5_2C7B9F0D3E61 */
