// The hover picker: every picker tab drawn into the level viewport, in 2D,
// as usdRig's pickerHoverUI.py draws it in usdview.
//
// Each tab is a small round handle with its buttons hanging off it, painted
// straight over the viewport with no window, frame or background around
// them. Everything that is not a button, a handle or the opacity knob stays
// the viewport's: a click there selects, box-selects, drags a gizmo or moves
// the camera exactly as it would without the picker.
//
// Gestures:
//
//   * handle, drag          -- move the tab; its buttons follow
//   * handle, Shift+drag    -- scale the tab about its handle
//   * handle, double-click  -- collapse the tab to its handle, or expand it
//   * button, click         -- select, with the docked picker's modifiers
//                              (Shift toggles, Ctrl removes, Alt mirrors)
//   * button, drag          -- marquee-select within that tab
//   * button or handle, middle-drag left/right -- the tab's opacity
//   * knob (bottom right), drag left/right -- every tab's opacity at once;
//                              double-click puts it back to full
//   * P over a level viewport -- show or hide the hover picker
//
// Hovering a button lights it as the docked picker does, and hovering a
// handle names its tab.
//
// Drawn the way usdview's is: a paint-only overlay on each level viewport
// (the mouse passes through it), with an input pre-processor deciding which
// presses are ours. Only buttons are drawn: the docked picker's backdrops
// stay in the docked picker, since over the viewport they would only hide
// the scene. The buttons, their state and what a click does belong to the
// shared picker model (FRigExecPickerModel), so the two pickers cannot
// disagree. The layout is a per-user preference (RigExecHover::FLayout),
// never written to the level.
#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "RigExecHoverLayout.h"
#include "Widgets/SLeafWidget.h"

class SLevelViewport;

/** The paint surface over one level viewport: transparent to the mouse, and
 * to the eye except where the hover picker draws. */
class SRigExecHoverOverlay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SRigExecHoverOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedPtr<SLevelViewport>& InViewport);
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	                      FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;

	TWeakPtr<SLevelViewport> Viewport;
};

class FRigExecHoverPicker : public IInputProcessor
{
public:
	/** Installs the pre-processor (the P key works from then on) and shows
	 * the picker if it was on when the user last left it. */
	static void Startup();
	static void Shutdown();
	static void SetEnabled(bool bEnabled);
	static bool IsEnabled();
	/** Whether a screen position is over a hover-picker button, handle or
	 * knob: Touch Pose leaves those to the picker. */
	static bool IsOver(const FVector2D& ScreenPosition);
	/** The controls the hover-picker button under a screen position
	 * selects, and their character; empty when it is over no such button.
	 * What a right-click there acts on. */
	static TArray<FName> ControlsAt(const FVector2D& ScreenPosition, class ARigExecActor*& OutActor);

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("RigExecHoverPicker"); }

	/** Paints the picker into one viewport's overlay. */
	int32 Paint(const SRigExecHoverOverlay& Overlay, const FGeometry& Geometry, FSlateWindowElementList& Out,
	            int32 LayerId) const;

private:
	/** One picker panel as the hover picker shows it. */
	struct FTab
	{
		int32 Panel = INDEX_NONE;
		FString Key;
		FString Label;
		/** The corner of the box around the buttons the tab can show, in
		 * either half of every IK/FK switch, and its size: panel units. */
		FVector2f Origin = FVector2f::ZeroVector;
		FVector2f Content = FVector2f(1.0f, 1.0f);
	};
	enum class EHitKind : uint8 { None, Knob, Handle, Button };
	struct FHit
	{
		EHitKind Kind = EHitKind::None;
		int32 Tab = INDEX_NONE;
		int32 Button = INDEX_NONE;
		bool operator==(const FHit& Other) const
		{
			return Kind == Other.Kind && Tab == Other.Tab && Button == Other.Button;
		}
	};
	enum class EGesture : uint8 { None, Knob, Move, Scale, Fade, Pick };
	struct FGesture
	{
		EGesture Kind = EGesture::None;
		FKey Mouse;
		int32 Tab = INDEX_NONE;
		TWeakPtr<SRigExecHoverOverlay> Overlay;
		FVector2f Start = FVector2f::ZeroVector;
		FVector2f Last = FVector2f::ZeroVector;
		float StartValue = 1.0f;
		FVector2f PanelStart = FVector2f::ZeroVector;
		int32 Mode = 0;
		bool bMirror = false;
		bool bBand = false;
	};
	struct FViewportOverlay
	{
		TWeakPtr<SLevelViewport> Viewport;
		TSharedPtr<SRigExecHoverOverlay> Widget;
	};

	/** Adds the overlay to every level viewport while on, and takes it off
	 * every one while off. */
	void SyncOverlays();
	/** Remakes the tabs when the picker reloaded. */
	void SyncTabs();
	/** The overlay of the level viewport under the cursor, and the cursor
	 * in its local space. */
	TSharedPtr<SRigExecHoverOverlay> OverlayAt(const FVector2D& ScreenPosition, FVector2f& OutLocal) const;
	/** What is under a viewport point, topmost first: the knob, then the
	 * tabs in reverse paint order. */
	FHit HitAt(const FVector2f& Point, const FVector2f& ViewSize) const;
	void SetHover(const FHit& Hit, const TSharedPtr<SRigExecHoverOverlay>& Overlay);
	bool Press(const FPointerEvent& MouseEvent, bool bDouble);
	void FinishPick(const FVector2D& ScreenAt);

	void PaintHandle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
	                 const RigExecHover::FTabLayout& Tab, bool bHovered, float Opacity) const;
	void PaintKnob(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, bool bHovered) const;
	void PaintText(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& At,
	               const FString& Text, bool bAlignRight, float Opacity) const;

	RigExecHover::FLayout Layout;
	TArray<FTab> Tabs;
	int32 Generation = -1;
	TArray<FViewportOverlay> Overlays;
	FHit Hover;
	TWeakPtr<SRigExecHoverOverlay> HoverOverlay;
	FGesture Gesture;
	/** The band's far corner, viewport space. */
	FVector2f BandEnd = FVector2f::ZeroVector;
	/** A press we took: its release is ours too. */
	FKey SwallowUp;
	bool bKeyLatched = false;
	double NextBind = 0.0;
};
