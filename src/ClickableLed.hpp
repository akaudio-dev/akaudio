#pragma once
#include <rack.hpp>

#include <algorithm>
#include <functional>

using namespace rack;

// A round status LED that is also a button. Color reflects playback state:
//   green = live (playing), amber = connecting/buffering, red = stopped.
// Left-click toggles playback via the owner-supplied callback (e.g. stop when
// playing → the LED turns red). Drawn emissive (drawLayer) so it glows like a
// real Rack light; a faint ring on hover hints that it's clickable.
struct ClickableLed : widget::OpaqueWidget {
	std::function<bool()> isLive;    // true → green
	std::function<bool()> isPending; // true → amber (overrides green when not yet live)
	std::function<void()> onToggle;  // left-click action
	bool hovered = false;

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
			if (onToggle)
				onToggle();
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}

	NVGcolor ledColor() {
		if (isPending && isPending())
			return nvgRGB(0xe0, 0xc0, 0x3a); // amber
		if (isLive && isLive())
			return nvgRGB(0x3a, 0xd0, 0x6a); // green
		return nvgRGB(0xe0, 0x4a, 0x3a);     // red (stopped)
	}

	void draw(const DrawArgs& args) override {
		float r = std::min(box.size.x, box.size.y) / 2.f;
		math::Vec c = box.size.div(2);
		// Dark housing + subtle border.
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, r);
		nvgFillColor(args.vg, nvgRGB(0x0e, 0x12, 0x16));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, hovered ? 0x60 : 0x22));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
		OpaqueWidget::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			float r = std::min(box.size.x, box.size.y) / 2.f;
			math::Vec c = box.size.div(2);
			NVGcolor col = ledColor();
			// Lit core.
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r * 0.72f);
			nvgFillColor(args.vg, col);
			nvgFill(args.vg);
			// Halo.
			nvgBeginPath(args.vg);
			nvgRect(args.vg, c.x - r * 3.f, c.y - r * 3.f, r * 6.f, r * 6.f);
			nvgFillPaint(args.vg, nvgRadialGradient(args.vg, c.x, c.y, r * 0.7f, r * 2.4f,
				nvgTransRGBAf(col, 0.55f), nvgTransRGBAf(col, 0.f)));
			nvgFill(args.vg);
		}
		OpaqueWidget::drawLayer(args, layer);
	}
};
