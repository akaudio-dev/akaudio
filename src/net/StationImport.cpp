// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "StationImport.hpp"
#include "Http.hpp"
#include "ImageCache.hpp"
#include "Json.hpp"
#include "Log.hpp"
#include "Stream.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace akaudio {

namespace {

// Verification gate: how many stereo frames must flow before we trust the stream.
// ~30k frames is ~0.7 s of real audio at 44.1/48 kHz, drained at playback speed.
constexpr uint64_t kMinFrames = 30000;
constexpr int kMaxPolls = 100; // × 100 ms = 10 s ceiling

std::string urlEncode(const std::string& s) {
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char) c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xf]; }
	}
	return out;
}

// FNV-1a hex, for a stable favicon cache filename keyed on the stream URL.
std::string hashKey(const std::string& s) {
	uint32_t h = 2166136261u;
	for (char c : s)
		h = (h ^ (unsigned char) c) * 16777619u;
	char buf[9];
	std::snprintf(buf, sizeof(buf), "%08x", h);
	return buf;
}

} // namespace

StationImporter::~StationImporter() {
	// Abort an audition in flight — without this, deleting the module mid-import
	// blocks the UI thread on the join for the verify poll + HTTP timeouts.
	abort_.store(true, std::memory_order_release);
	if (thread.joinable())
		thread.join();
}

void StationImporter::start(const std::string& url, std::function<Probe()> probe,
		const std::string& cacheDir) {
	if (running_.exchange(true, std::memory_order_acq_rel))
		return; // one at a time
	if (thread.joinable())
		thread.join();
	{
		std::lock_guard<std::mutex> lock(mutex);
		url_ = url;
		probe_ = std::move(probe);
		cacheDir_ = cacheDir;
		status_ = "Auditioning…";
	}
	thread = std::thread(&StationImporter::run, this);
}

void StationImporter::cancel() {
	abort_.store(true, std::memory_order_release);
	if (thread.joinable())
		thread.join();
	// Reusable again. run() already set running_=false in finish(); the generation it
	// bumped is harmless — the caller drops the stale result by URL.
	abort_.store(false, std::memory_order_release);
	running_.store(false, std::memory_order_release);
}

std::string StationImporter::status() {
	std::lock_guard<std::mutex> lock(mutex);
	return status_;
}

StationImporter::Result StationImporter::result() {
	std::lock_guard<std::mutex> lock(mutex);
	return result_;
}

void StationImporter::run() {
	std::string url, cacheDir;
	std::function<Probe()> probe;
	{
		std::lock_guard<std::mutex> lock(mutex);
		url = url_; cacheDir = cacheDir_; probe = probe_;
	}

	auto setStatus = [&](const std::string& s) {
		std::lock_guard<std::mutex> lock(mutex);
		status_ = s;
	};
	auto finish = [&](bool ok, bool identified, const std::string& name,
			const std::string& icon, const std::string& status) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			Result r;
			r.ok = ok;
			r.identified = identified;
			r.url = url;
			r.name = name;
			r.iconPath = icon;
			r.status = status;
			result_ = r;
			status_ = status;
		}
		if (!ok) // successes are the expected case; only failures are log-worthy
			netLog("audition FAILED (" + status + "): " + url);
		generation_.fetch_add(1, std::memory_order_acq_rel);
		running_.store(false, std::memory_order_release);
	};

	// 1. Verify: wait for decoded audio to actually flow, not just a connection.
	uint64_t startFrames = 0;
	bool started = false, verified = false;
	std::string errText;
	bool sawError = false, sawPlaying = false;
	for (int i = 0; i < kMaxPolls && !abort_.load(std::memory_order_acquire); i++) {
		Probe p;
		if (probe)
			p = probe();
		else
			p.state = StreamClient::State::Error;
		if (p.state == StreamClient::State::Error) {
			sawError = true;
			errText = p.status;
			break;
		}
		if (p.state == StreamClient::State::Playing)
			sawPlaying = true;
		if (p.frames > 0 && !started) {
			started = true;
			startFrames = p.frames;
		}
		if (started && p.frames - startFrames >= kMinFrames) {
			verified = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	if (!verified) {
		std::string reason;
		if (sawError)
			reason = errText.empty() ? "Couldn't connect" : errText;
		else if (started)
			reason = "Audio dropped out";
		else if (sawPlaying)
			reason = "Connected, but no audio";
		else
			reason = "Couldn't connect (timed out)";
		finish(false, false, "", "", reason);
		return;
	}

	// 2. Identify via radio-browser (byurl). Best-effort.
	setStatus("Identifying…");
	std::string name, favicon;
	{
		std::string body;
		std::string api = "https://all.api.radio-browser.info/json/stations/byurl?url=" + urlEncode(url);
		if (httpGet(api, body, &abort_)) {
			json_error_t err;
			json_t* root = json_loads(body.c_str(), 0, &err);
			if (root && json_is_array(root) && json_array_size(root) > 0) {
				json_t* s = json_array_get(root, 0);
				name = flexStr(json_object_get(s, "name"));
				favicon = flexStr(json_object_get(s, "favicon"));
			}
			if (root)
				json_decref(root);
		}
	}
	bool identified = !name.empty();

	// 3. Favicon: radio-browser's only, and only if it decodes to a raster image
	// (PNG/JPG/ICO→BMP). SVG and missing favicons fall through to a synth avatar.
	std::string iconPath;
	if (!favicon.empty()) {
		setStatus("Fetching art…");
		std::string body;
		if (httpGet(favicon, body, &abort_) && body.size() >= 16)
			iconPath = cacheImage(body, cacheDir, hashKey(url));
	}

	if (identified)
		finish(true, true, name, iconPath, "Added \xe2\x80\x9c" + name + "\xe2\x80\x9d");
	else
		finish(true, false, "", iconPath, "Playing \xe2\x80\x94 name it to save");
}

} // namespace akaudio
