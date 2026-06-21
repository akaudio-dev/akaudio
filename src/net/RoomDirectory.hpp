#pragma once
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

// Background directory of public NINJAM jam rooms.
//
// Source: http://ninbot.com/app/servers.php — a JSON {servers:[…]} listing of
// live rooms, each carrying its Icecast listen-stream URLs (the same endpoint
// jamauv3's ServerBrowser uses). We only consume the rooms here; listening is
// done by pointing StreamClient at a room's http stream URL.
//
// THREADING: refresh() fetches+parses on a background thread so the UI thread
// never blocks on the network. The UI reads the cached room list instantly via
// rooms(). All public methods are meant for the UI thread.

namespace akaudio {

struct Room {
	std::string name;
	std::string host;
	int port = 0;
	int bpm = 0;
	int bpi = 0;
	int userCount = 0; // active players in the room
	int userMax = 0;   // capacity (0 = unknown)
	int pri = 999;     // listing priority (lower = more prominent)
	std::string stream;    // http:// Icecast MP3 mount
	std::string sslStream; // https:// variant (StreamClient does TLS now)
	std::vector<std::string> users; // player display names

	// The URL StreamClient should play: prefer the plain-http mount (slightly
	// cheaper), fall back to the https one. Empty if the room has neither.
	const std::string& playUrl() const { return !stream.empty() ? stream : sslStream; }
	bool playable() const { return !stream.empty() || !sslStream.empty(); }
};

class RoomDirectory {
public:
	RoomDirectory() = default;
	~RoomDirectory();

	RoomDirectory(const RoomDirectory&) = delete;
	RoomDirectory& operator=(const RoomDirectory&) = delete;

	// Kick a background fetch. No-op if one is already in flight. Returns
	// immediately; results land in the cache when the fetch completes.
	void refresh();

	// Snapshot of the cached rooms, sorted by user count (desc) then priority.
	std::vector<Room> rooms();

	bool loading() const { return loading_.load(std::memory_order_acquire); }
	std::string status();

	// Bumped on every successful fetch+parse. The UI compares it against a
	// remembered value to know when to rebuild its view (cheap, no locking).
	unsigned generation() const { return generation_.load(std::memory_order_acquire); }

private:
	void fetch(unsigned bust);

	std::mutex mutex;
	std::vector<Room> rooms_;
	std::string status_ = "Not loaded";

	std::thread thread;
	std::atomic<bool> loading_{false};
	std::atomic<unsigned> bust_{0};       // cache-bust counter for ?t=
	std::atomic<unsigned> generation_{0}; // bumped on each successful parse
};

} // namespace akaudio
