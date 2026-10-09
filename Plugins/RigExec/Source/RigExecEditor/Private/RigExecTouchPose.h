// TouchPose in the level viewport: the character's own skin as the control
// picker, as usdview's TouchPose plugin does it. Hovering a touch region
// lights it; a click selects the control it names (Shift toggles, Ctrl
// removes); the regions of selected controls stay lit, the last selected in
// the lead colour. A manipulator always wins: clicks on the transform
// gizmo (new or legacy), a control shape or anything in front of the skin go
// to the viewport as usual. While the rig is being worked (a drag, playback,
// an attribute edit) the mode steps aside until it settles.
#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"

class ARigExecActor;
class FLevelEditorViewportClient;

class FRigExecTouchPose : public IInputProcessor
{
public:
	/** Turns the mode on or off; off clears every highlight. */
	static void SetEnabled(bool bEnabled);
	static bool IsEnabled();
	/** Unregisters, at module shutdown. */
	static void Shutdown();
	/** The control of the touch region under a screen position, and its
	 * character, while Touch Pose is on and the skin is what is under the
	 * cursor; None otherwise. What a right-click there acts on. */
	static FName ControlAt(const FVector2D& ScreenPosition, ARigExecActor*& OutActor);

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("RigExecTouchPose"); }

private:
	struct FHit
	{
		TWeakObjectPtr<ARigExecActor> Actor;
		int32 Region = -1;
		FLevelEditorViewportClient* Client = nullptr;
		FIntPoint Pixel = FIntPoint::ZeroValue;
		/** The pick ray, world space, and where it left the viewport. */
		FVector Origin = FVector::ZeroVector;
		FVector Direction = FVector::ZeroVector;
		FVector2D ScreenPixel = FVector2D::ZeroVector;
	};
	/** The touch region under the screen position, if the cursor is over a
	 * level viewport. */
	FHit HitAt(const FVector2D& ScreenPosition) const;
	/** Lights hover and selection on every character in the level. */
	void Highlight(const FHit& Hover) const;
	void ClearAll() const;

	bool bSwallowUp = false;
	FVector2D LastCursor = FVector2D(-1.0, -1.0);
};
