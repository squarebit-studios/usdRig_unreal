// The right-click marking menu for rig controls in the level viewport, as
// usdRig's markingMenuUI.py does it in usdview.
//
// Right-click over a control -- its shape in the viewport, a Touch Pose
// region (which stands for its control) or a hover-picker button -- and
// this menu takes the click:
//
//   * flick toward a direction and release: the command runs, nothing is
//     ever drawn;
//   * hold: the ring appears around the press point, and releasing over an
//     item runs it;
//   * a quick tap leaves the ring up to be clicked; Escape or a click away
//     from it dismisses it;
//   * a submenu leaves a circle where each menu above it was centred, and
//     moving onto one goes back to that menu;
//   * a label or a row of the column below the ring is taken whenever the
//     cursor is on it, held or clicked, ahead of the direction.
//
// The menu acts on the selection. Right-clicking a control that is not
// selected selects it first, so what the menu acts on is always what is
// highlighted. A control under the cursor wins over the camera whatever the
// modifiers, and while controls are selected a plain right-click anywhere in
// the viewport opens the menu for them: both are intentional. Only a
// right-click with nothing selected, or Alt over empty space (the camera's
// dolly), stays the viewport's.
//
// The menu is drawn in an overlay on the whole editor window, above every
// panel, so a viewport edge never cuts it off; a label that would run off the
// window slides back on.
//
// What the gesture means is RigExecMarking's (RigExecMarkingMenuModel);
// this paints, routes the mouse and runs the commands the item ids name.
#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Widgets/SLeafWidget.h"

class ARigExecActor;
class SLevelViewport;
class SViewport;
class FRigExecMarkingMenuController;

/** The paint surface: an overlay on the editor window. */
class SRigExecMarkingOverlay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SRigExecMarkingOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	                      FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;
};

class FRigExecMarkingMenu : public IInputProcessor
{
public:
	/** Installs the right-click handling, ahead of the hover picker and
	 * Touch Pose. */
	static void Startup();
	static void Shutdown();
	static bool IsOpen();

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("RigExecMarkingMenu"); }

	int32 Paint(const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId) const;

	FRigExecMarkingMenu();
	virtual ~FRigExecMarkingMenu();

private:
	TUniquePtr<FRigExecMarkingMenuController> Controller;
	/** A press taken: its release is ours too, even after the menu closed. */
	FKey SwallowUp;
};
