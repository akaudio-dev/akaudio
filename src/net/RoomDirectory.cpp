// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "RoomDirectory.hpp"
#include "Http.hpp"

#include <algorithm>

#include <jansson.h>

namespace akaudio {

namespace {

const char* kApiUrl = "http://ninbot.com/app/servers.php";

// ninbot returns some numeric fields as JSON strings, others as integers.
// Accept either (mirrors jamauv3's FlexInt).
int flexInt(json_t* v, int fallback = 0) {
	if (!v)
		return fallback;
	if (json_is_integer(v))
		return (int) json_integer_value(v);
	if (json_is_real(v))
		return (int) json_real_value(v);
	if (json_is_string(v)) {
		const char* s = json_string_value(v);
		if (s && *s)
			return std::atoi(s);
	}
	return fallback;
}

std::string flexStr(json_t* v) {
	if (v && json_is_string(v))
		return json_string_value(v);
	return "";
}

} // namespace

RoomDirectory::~RoomDirectory() {
	if (thread.joinable())
		thread.join();
}

void RoomDirectory::refresh() {
	if (loading_.exchange(true, std::memory_order_acq_rel))
		return; // a fetch is already running
	// The previous fetch (if any) has finished — loading_ guards that — so the
	// join here is instant and just reclaims the old thread.
	if (thread.joinable())
		thread.join();
	{
		std::lock_guard<std::mutex> lock(mutex);
		status_ = "Loading…";
	}
	thread = std::thread(&RoomDirectory::fetch, this, bust_.fetch_add(1) + 1);
}

std::vector<Room> RoomDirectory::rooms() {
	std::lock_guard<std::mutex> lock(mutex);
	return rooms_;
}

std::string RoomDirectory::status() {
	std::lock_guard<std::mutex> lock(mutex);
	return status_;
}

void RoomDirectory::fetch(unsigned bust) {
	std::string url = std::string(kApiUrl) + "?t=" + std::to_string(bust);
	std::string body;
	bool ok = httpGet(url, body);

	std::vector<Room> parsed;
	std::string status;

	if (!ok) {
		status = "Fetch failed";
	}
	else {
		json_error_t err;
		json_t* root = json_loads(body.c_str(), 0, &err);
		json_t* servers = root ? json_object_get(root, "servers") : nullptr;
		if (!servers || !json_is_array(servers)) {
			status = "Bad response";
		}
		else {
			size_t i;
			json_t* s;
			json_array_foreach(servers, i, s) {
				if (!json_is_object(s))
					continue;
				Room r;
				r.name = flexStr(json_object_get(s, "name"));
				r.host = flexStr(json_object_get(s, "host"));
				r.port = flexInt(json_object_get(s, "port"));
				r.bpm = flexInt(json_object_get(s, "bpm"));
				r.bpi = flexInt(json_object_get(s, "bpi"));
				r.pri = flexInt(json_object_get(s, "pri"), 999);
				r.stream = flexStr(json_object_get(s, "stream"));
				r.sslStream = flexStr(json_object_get(s, "ssl_stream"));

				// user_max / user_limit give capacity; user count is the players list.
				r.userMax = flexInt(json_object_get(s, "user_max"));
				if (r.userMax == 0)
					r.userMax = flexInt(json_object_get(s, "user_limit"));

				json_t* users = json_object_get(s, "users");
				if (users && json_is_array(users)) {
					size_t j;
					json_t* u;
					json_array_foreach(users, j, u) {
						std::string nm = flexStr(json_object_get(u, "name"));
						if (!nm.empty())
							r.users.push_back(nm);
					}
				}
				r.userCount = (int) r.users.size();

				if (!r.name.empty())
					parsed.push_back(std::move(r));
			}
			status = "Loaded " + std::to_string(parsed.size()) + " rooms";
		}
		if (root)
			json_decref(root);
	}

	// Sort by active players (desc), then listing priority (asc).
	std::sort(parsed.begin(), parsed.end(), [](const Room& a, const Room& b) {
		if (a.userCount != b.userCount)
			return a.userCount > b.userCount;
		return a.pri < b.pri;
	});

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (ok)
			rooms_ = std::move(parsed);
		status_ = status;
	}
	if (ok)
		generation_.fetch_add(1, std::memory_order_acq_rel);
	loading_.store(false, std::memory_order_release);
}

} // namespace akaudio
