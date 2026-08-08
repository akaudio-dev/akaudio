// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <rack.hpp>

#include "Theme.hpp"

#include <algorithm>
#include <functional>

using namespace rack;

// Shared NanoVG drawing for the round status LED, reused by the plain
// callback-driven ClickableLed and the MIDI-mappable MomentaryLed below.
static inline void akDrawLedHousing(NVGcontext* vg, math::Vec size, bool hovered) {
	float r = std::min(size.x, size.y) / 2.f;
	math::Vec c = size.div(2);
	// Dark housing + subtle border (brighter on hover to hint it's clickable).
	nvgBeginPath(vg);
	nvgCircle(vg, c.x, c.y, r);
	nvgFillColor(vg, nvgRGB(0x0e, 0x12, 0x16));
	nvgFill(vg);
	nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, hovered ? 0x60 : 0x22));
	nvgStrokeWidth(vg, 1.f);
	nvgStroke(vg);
}

static inline void akDrawLedGlow(NVGcontext* vg, math::Vec size, NVGcolor col) {
	float r = std::min(size.x, size.y) / 2.f;
	math::Vec c = size.div(2);
	// Lit core.
	nvgBeginPath(vg);
	nvgCircle(vg, c.x, c.y, r * 0.72f);
	nvgFillColor(vg, col);
	nvgFill(vg);
	// Halo.
	nvgBeginPath(vg);
	nvgRect(vg, c.x - r * 3.f, c.y - r * 3.f, r * 6.f, r * 6.f);
	nvgFillPaint(vg, nvgRadialGradient(vg, c.x, c.y, r * 0.7f, r * 2.4f,
		nvgTransRGBAf(col, 0.55f), nvgTransRGBAf(col, 0.f)));
	nvgFill(vg);
}

// Map the (pending, live) status pair to the LED color:
//   green = live (playing), amber = connecting/buffering, red = stopped.
static inline NVGcolor akLedColor(bool pending, bool live) {
	if (pending)
		return nvgRGB(0xe0, 0xc0, 0x3a); // amber
	if (live)
		return AK_LED_GREEN;
	return nvgRGB(0xe0, 0x4a, 0x3a);     // red (stopped)
}

// A round status LED that is also a button. Left-click toggles the owning
// module's state via the owner-supplied callback (`onClick`, from HoverButton).
// Drawn emissive (drawLayer) so it glows like a real Rack light. Not a
// ParamWidget — used where the click drives an action that is not (and should
// not be) MIDI-mappable (e.g. Ninjam's stop LED, whose stopAll() joins network
// threads and must never run off the audio thread). For a mappable button see
// MomentaryLed below.
struct ClickableLed : HoverButton {
	std::function<bool()> isLive;    // true → green
	std::function<bool()> isPending; // true → amber (overrides green when not yet live)

	NVGcolor ledColor() {
		return akLedColor(isPending && isPending(), isLive && isLive());
	}

	void draw(const DrawArgs& args) override {
		akDrawLedHousing(args.vg, box.size, hovered);
		OpaqueWidget::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			akDrawLedGlow(args.vg, box.size, ledColor());
		OpaqueWidget::drawLayer(args, layer);
	}
};

// Same LED visuals, but backed by an engine Param so it can be MIDI-mapped
// (MIDI-Map learns the last-touched ParamWidget). Two-state switch; the owning
// widget reconciles it with the module's real state on the UI thread and sets
// `momentary` to match the module's MIDI mode: latch (value = on/off state — a
// CC/latching button/click drive it intuitively and in sync) or momentary
// (toggle on each rising edge — a note/pad toggles once per press). Bind with
// createParam<ParamLed>(pos, module, SOME_PARAM). The LED colour still reflects
// live stream state (isLive/isPending), not the raw param value.
struct ParamLed : app::Switch {
	bool hovered = false;
	std::function<bool()> isLive;    // true → green
	std::function<bool()> isPending; // true → amber

	NVGcolor ledColor() {
		return akLedColor(isPending && isPending(), isLive && isLive());
	}

	void onEnter(const EnterEvent& e) override {
		hovered = true;
		app::Switch::onEnter(e);
	}
	void onLeave(const LeaveEvent& e) override {
		hovered = false;
		app::Switch::onLeave(e);
	}

	void draw(const DrawArgs& args) override {
		akDrawLedHousing(args.vg, box.size, hovered);
		app::Switch::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			akDrawLedGlow(args.vg, box.size, ledColor());
		app::Switch::drawLayer(args, layer);
	}
};

// Reconcile a mappable two-state Param with a module's real boolean state, on
// the UI thread (call once per frame from the widget's step()). Resolves the
// note-vs-CC ambiguity via `momentaryMode`:
//   - latch (momentaryMode=false): the param value IS the state. A CC/latching
//     button/click drives it directly; internal state changes reflect back so
//     the two stay mirrored (great for CCs, in sync).
//   - momentary (momentaryMode=true): a rising edge toggles the state and the
//     param is left free-running (great for notes/pads — one toggle per press).
// getState() reads the current state; setState(desired) applies it (may be a
// no-op if already there). Holds its own cross-frame caches and re-inits on a
// mode change so switching modes never emits a phantom toggle.
struct ParamStateSync {
	bool inited = false;
	bool lastMode = false;
	bool lastParamHi = false;
	bool lastState = false;

	template <class Get, class Set>
	void reconcile(engine::Param& param, bool momentaryMode, Get getState, Set setState) {
		bool paramHi = param.getValue() > 0.5f;
		bool state = getState();
		if (!inited || momentaryMode != lastMode) {
			inited = true;
			lastMode = momentaryMode;
			lastState = state;
			if (momentaryMode) {
				lastParamHi = paramHi;              // adopt, don't act
			} else {
				param.setValue(state ? 1.f : 0.f);  // latch: reflect state now
				lastParamHi = state;
			}
			return;
		}
		if (momentaryMode) {
			if (paramHi && !lastParamHi)
				setState(!state);                   // toggle on rising edge
			lastParamHi = paramHi;
		} else {
			if (paramHi != lastParamHi) {           // param moved (CC or click)
				lastParamHi = paramHi;
				if (paramHi != state)
					setState(paramHi);
			}
			if (state != lastState) {               // state changed elsewhere → mirror
				lastState = state;
				param.setValue(state ? 1.f : 0.f);
				lastParamHi = state;
			}
		}
	}
};
