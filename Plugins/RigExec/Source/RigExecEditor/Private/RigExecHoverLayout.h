// The hover picker's layout and gestures, with no Slate in sight: a port of
// usdRig's pickerHoverModel.py.
//
// The hover picker draws each picker tab straight into the level viewport as
// a group of buttons hanging off a small circle (the tab's handle), in 2D
// screen space. This owns where each group is, how big, how opaque, and
// whether it is collapsed, plus the arithmetic the gestures apply to those.
// The drawing and the mouse live in FRigExecHoverPicker.
//
// Coordinates are the viewport's Slate units, origin top-left. A tab's
// (X, Y) is the centre of its handle; its buttons hang below it with their
// top-left corner under the handle's left edge, at the tab's scale.
//
// The layout is a viewer preference, never scene data: it is saved per user
// (Saved/RigExec/HoverPicker.json), keyed by the picker and the tab it shows.
#pragma once

#include "CoreMinimal.h"

namespace RigExecHover
{
// The handle's radius and the gap between it and the buttons. Neither scales
// with the tab: the handle has to stay grabbable however small the buttons
// are made.
constexpr float HandleRadius = 7.0f;
constexpr float HandleGap = 6.0f;
// Grabbing slop around the handle, so a 14 px circle is not a precision
// target.
constexpr float HandleSlop = 3.0f;

constexpr float MinScale = 0.2f;
constexpr float MaxScale = 4.0f;
constexpr float MinOpacity = 0.05f;
constexpr float MaxOpacity = 1.0f;
// Shift-drag on a handle: the scale multiplies by exp(rate * px), so equal
// drags give equal ratios whatever the current size.
constexpr float ScaleRate = 0.006f;
// Middle-drag: a full sweep from faint to solid is 200 px.
constexpr float OpacityRate = 1.0f / 200.0f;

// Where the first handle goes the first time the hover picker is shown, and
// how far apart the handles sit: below the level viewport's toolbar, so a
// fresh layout never sits on its buttons.
constexpr float FirstHandleX = 24.0f;
constexpr float FirstHandleY = 64.0f;
constexpr float HandleSpacing = 26.0f;
// A fresh tab is sized so its buttons take at most this fraction of the
// viewport's height, and never more than its authored size.
constexpr float FitFraction = 0.45f;

// The overall-opacity knob, bottom-right, inset from the corner.
constexpr float KnobRadius = 8.0f;
constexpr float KnobInset = 18.0f;
constexpr float KnobBottomClearance = 36.0f;

/** The key a tab's layout is saved under. */
FString TabKey(const FString& Picker, const FString& Panel);

/** The scale a fresh tab starts at. */
float FitScale(float ContentHeight, float ViewHeight);

/** Where the overall-opacity knob sits in a viewport this size. */
FVector2f KnobCentre(const FVector2f& ViewSize);
bool HitsKnob(const FVector2f& ViewSize, const FVector2f& Point);

/** One tab's place on screen. */
struct FTabLayout
{
	float X = 0.0f;
	float Y = 0.0f;
	float Scale = 1.0f;
	float Opacity = 1.0f;
	bool bCollapsed = false;

	/** The screen point the tab's content origin is drawn at. */
	FVector2f ButtonsCorner() const;
	/** A panel point as a screen point; `Origin` is the corner of the
	 * tab's content in panel units. */
	FVector2f ToScreen(const FVector2f& Origin, const FVector2f& Point) const;
	/** The inverse of ToScreen. */
	FVector2f ToPanel(const FVector2f& Origin, const FVector2f& Point) const;
	bool HitsHandle(const FVector2f& Point) const;

	void Moved(const FVector2f& Delta);
	/** Shift-drag from where it started: right or up grows, left or down
	 * shrinks. The handle stays put; the buttons follow it. */
	void Scaled(float StartScale, const FVector2f& Delta);
	/** Middle-drag from where it started: right is more opaque. */
	void Faded(float StartOpacity, float DeltaX);
};

/** Every tab's place, the overall opacity, and whether hover is on. */
struct FLayout
{
	TMap<FString, FTabLayout> Tabs;
	float Opacity = 1.0f;
	bool bEnabled = false;

	/** Gives every key that has no layout yet one: handles side by side
	 * along the top, right of anything already placed in that row, all
	 * collapsed but the first tab of a fresh layout. `FitScales` maps a key
	 * to the scale it should start at. */
	void Ensure(const TArray<FString>& Keys, const TMap<FString, float>& FitScales);
	/** The overall opacity, from a drag on the knob. */
	void Faded(float StartOpacity, float DeltaX);
	/** What a tab is drawn at: its own opacity times the overall. */
	float TabOpacity(const FString& Key) const;

	FString ToJson() const;
	static FLayout FromJson(const FString& Text);

	/** The user's saved layout, or a fresh one. */
	static FLayout Load();
	void Save() const;
};
} // namespace RigExecHover
