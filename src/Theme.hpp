// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#pragma once
#include <rack.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace rack;

// Shared house style for the AK Audio panels (Radio + Ninjam): fonts, the colors
// and output-plate geometry both panels must agree on (the plates are meant to
// read as one plugin family — a change here moves both), and the text/button
// helpers every widget uses.

// ---- Fonts (Rack system assets) ----
static const char* FONT_BOLD = "res/fonts/Nunito-Bold.ttf";
static const char* FONT_REG = "res/fonts/DejaVuSans.ttf";
static const char* FONT_MONO = "res/fonts/ShareTechMono-Regular.ttf"; // TTY chat console

// ---- Shared palette ----
static const NVGcolor AK_PLATE      = nvgRGB(0x1f, 0x1f, 0x1f); // output-plate black (core/Fundamental)
static const NVGcolor AK_PLATE_TEXT = nvgRGB(0xf0, 0xf0, 0xf0); // labels on the dark plate (AUDIO #f0f0f0)
static const NVGcolor AK_LED_GREEN  = nvgRGB(0x3a, 0xd0, 0x6a); // live LED / current-station ring

// ---- Output-plate geometry (mm) ----
// Fundamental: 39.16 px tall, r 2.83 px on a 75 px panel → 13.26 mm / 0.96 mm.
// The x position / width are per-panel (the plate is centered on each panel);
// everything vertical is shared so the two modules' jack rows line up in a rack.
static const float AK_PLATE_TOP_MM = 104.66f;
static const float AK_PLATE_H_MM = 13.26f;
static const float AK_PLATE_R_MM = 0.96f;
static const float AK_PLATE_LABEL_DY_MM = 3.1f; // label baseline below the plate top
static const float AK_ROW_CV_MM = 96.859f;      // CV/input jack row above the plate
static const float AK_ROW_OUT_MM = 113.115f;    // output jack row on the plate
static const float AK_MARK_Y_MM = 121.0f;       // "AK" maker mark (16 pt bold)

// Load a Rack system font by resource path, caching the asset::system() path
// resolution (it heap-concatenates per call, and draw code runs per frame).
inline std::shared_ptr<window::Font> akLoadFont(const char* fontRes) {
	static std::map<const char*, std::string> paths; // UI thread only
	std::string& path = paths[fontRes];
	if (path.empty())
		path = asset::system(fontRes);
	return APP->window->loadFont(path);
}

// Draw left-aligned (or centered) text, vertically centered on y, optionally
// ellipsized to clipW px.
inline void drawTxt(NVGcontext* vg, const char* fontRes, float x, float y, float size,
		NVGcolor col, const std::string& s, int halign = NVG_ALIGN_LEFT, float clipW = -1.f) {
	std::shared_ptr<window::Font> font = akLoadFont(fontRes);
	if (!font || font->handle < 0)
		return;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, size);
	nvgFillColor(vg, col);

	if (clipW > 0.f) {
		// Measure under LEFT alignment: nvgTextGlyphPositions/nvgTextBounds shift the
		// reported x by the alignment (RIGHT subtracts the full width, CENTER half), so
		// measuring under the caller's `halign` would give <=0 positions and never clip.
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		float b[4];
		nvgTextBounds(vg, 0, 0, s.c_str(), NULL, b);
		if (b[2] - b[0] > clipW) {
			// One glyph-position pass finds the cut point (never splits a UTF-8
			// sequence — glyph starts are code-point starts).
			nvgTextBounds(vg, 0, 0, "\xe2\x80\xa6", NULL, b); // …
			const float fitW = clipW - (b[2] - b[0]);
			NVGglyphPosition pos[256];
			int n = nvgTextGlyphPositions(vg, 0, 0, s.c_str(), NULL, pos,
				(int) std::min<size_t>(s.size(), 256));
			size_t keep = 0;
			for (int i = 0; i < n; i++) {
				if (pos[i].maxx > fitW)
					break;
				const char* next = (i + 1 < n) ? pos[i + 1].str : s.c_str() + s.size();
				keep = (size_t) (next - s.c_str());
			}
			std::string t = s.substr(0, keep) + "\xe2\x80\xa6";
			nvgTextAlign(vg, halign | NVG_ALIGN_MIDDLE);
			nvgText(vg, x, y, t.c_str(), NULL);
			return;
		}
	}
	nvgTextAlign(vg, halign | NVG_ALIGN_MIDDLE);
	nvgText(vg, x, y, s.c_str(), NULL);
}

// Width in px of a string for a given font/size (for caret/column math).
inline float textWidth(NVGcontext* vg, const char* fontRes, float size, const std::string& s) {
	std::shared_ptr<window::Font> font = akLoadFont(fontRes);
	if (!font || font->handle < 0)
		return 0.f;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, size);
	float b[4];
	return nvgTextBounds(vg, 0, 0, s.c_str(), NULL, b);
}

// Hover-tracking click button: the shared skeleton behind the panels' small
// clickable widgets (status LED, steppers, toggles, room rows). Subclasses draw
// themselves and either assign `onClick` or override `onPress` for custom hit
// handling (an override must consume the event itself).
struct HoverButton : widget::OpaqueWidget {
	bool hovered = false;
	std::function<void()> onClick;

	void onEnter(const EnterEvent& e) override {
		hovered = true;
		OpaqueWidget::onEnter(e);
	}
	void onLeave(const LeaveEvent& e) override {
		hovered = false;
		OpaqueWidget::onLeave(e);
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			onPress(e);
			return;
		}
		OpaqueWidget::onButton(e);
	}
	virtual void onPress(const ButtonEvent& e) {
		if (onClick)
			onClick();
		e.consume(this);
	}
};
