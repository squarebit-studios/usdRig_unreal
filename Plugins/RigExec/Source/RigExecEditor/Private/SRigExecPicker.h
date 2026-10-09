// The control picker for a RigExec character in the level, docked as a tab:
// the shared picker model (FRigExecPickerModel) drawn one panel at a time,
// as the usdview picker draws it, selecting and switching the character's
// Control Rig.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

/** One panel, drawn and clickable. */
class SRigExecPickerCanvas : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SRigExecPickerCanvas) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, class SRigExecPicker* InOwner);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	                      const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	                      int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual void OnMouseLeave(const FPointerEvent& Event) override;

private:
	float Scale(const FGeometry& Geometry) const;
	FVector2f ToPanel(const FGeometry& Geometry, const FVector2D& Screen) const;

	class SRigExecPicker* Owner = nullptr;
	bool bPressed = false;
	bool bDragging = false;
	FVector2f PressAt, DragAt;
	int32 Hover = INDEX_NONE;
};

class SRigExecPicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRigExecPicker) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The panel the tabs show. */
	int32 GetPanelIndex() const { return PanelIndex; }

private:
	EActiveTimerReturnType Refresh(double, float);
	/** Rebinds the model, and remakes the tabs when it reloaded. */
	void Sync();

	int32 PanelIndex = 0;
	int32 Generation = -1;
	TSharedPtr<class SHorizontalBox> Tabs;
};
