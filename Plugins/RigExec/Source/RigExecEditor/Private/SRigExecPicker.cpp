#include "SRigExecPicker.h"

#include "RigExecPickerModel.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RigExecPicker"

// -- canvas ---------------------------------------------------------------

void
SRigExecPickerCanvas::Construct(const FArguments&, SRigExecPicker* InOwner)
{
	Owner = InOwner;
}

FVector2D
SRigExecPickerCanvas::ComputeDesiredSize(float) const
{
	const FRigExecPickerPanel* Panel = FRigExecPickerModel::Get().GetPanel(Owner->GetPanelIndex());
	return Panel ? FVector2D(Panel->Size) : FVector2D(400.0, 600.0);
}

float
SRigExecPickerCanvas::Scale(const FGeometry& Geometry) const
{
	const FRigExecPickerPanel* Panel = FRigExecPickerModel::Get().GetPanel(Owner->GetPanelIndex());
	if (!Panel)
	{
		return 1.0f;
	}
	const FVector2f Size = FVector2f(Geometry.GetLocalSize());
	return FMath::Max(FMath::Min(Size.X / Panel->Size.X, Size.Y / Panel->Size.Y), 1e-3f);
}

FVector2f
SRigExecPickerCanvas::ToPanel(const FGeometry& Geometry, const FVector2D& Screen) const
{
	return FVector2f(Geometry.AbsoluteToLocal(Screen)) / Scale(Geometry);
}

int32
SRigExecPickerCanvas::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
                              FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle&,
                              bool) const
{
	const FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	const FSlateBrush* White = FAppStyle::GetBrush("WhiteBrush");
	FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(), White, ESlateDrawEffect::None,
	                           FLinearColor(FColor(42, 42, 42)));
	const int32 PanelIndex = Owner->GetPanelIndex();
	const FRigExecPickerPanel* Panel = Model.GetPanel(PanelIndex);
	if (!Panel)
	{
		return LayerId;
	}
	const float S = Scale(Geometry);
	FSlateDrawElement::MakeBox(Out, ++LayerId,
	                           Geometry.ToPaintGeometry(Panel->Size * S, FSlateLayoutTransform()), White,
	                           ESlateDrawEffect::None, FLinearColor(Panel->Fill));
	for (int32 Index : Model.Visible(PanelIndex))
	{
		LayerId += 4;
		Model.PaintButton(Panel->Buttons[Index], Index == Hover, Geometry, FVector2f::ZeroVector, S, 1.0f, Out,
		                  LayerId);
	}
	if (bDragging)
	{
		const FVector2f Lo(FMath::Min(PressAt.X, DragAt.X), FMath::Min(PressAt.Y, DragAt.Y));
		const FVector2f Hi(FMath::Max(PressAt.X, DragAt.X), FMath::Max(PressAt.Y, DragAt.Y));
		FSlateDrawElement::MakeBox(Out, ++LayerId,
		                           Geometry.ToPaintGeometry((Hi - Lo) * S, FSlateLayoutTransform(Lo * S)), White,
		                           ESlateDrawEffect::None, FLinearColor(0.47f, 0.78f, 1.0f, 0.16f));
		TArray<FVector2f> Band = {Lo * S, FVector2f(Hi.X, Lo.Y) * S, Hi * S, FVector2f(Lo.X, Hi.Y) * S, Lo * S};
		FSlateDrawElement::MakeLines(Out, ++LayerId, Geometry.ToPaintGeometry(), Band, ESlateDrawEffect::None,
		                             FLinearColor(0.59f, 0.84f, 1.0f), true, 1.0f);
	}
	return LayerId;
}

FReply
SRigExecPickerCanvas::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bPressed = true;
	bDragging = false;
	PressAt = DragAt = ToPanel(Geometry, Event.GetScreenSpacePosition());
	return FReply::Handled().CaptureMouse(SharedThis(this));
}

FReply
SRigExecPickerCanvas::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
	const FVector2f At = ToPanel(Geometry, Event.GetScreenSpacePosition());
	if (bPressed)
	{
		DragAt = At;
		bDragging = bDragging || FVector2f::Distance(PressAt, DragAt) * Scale(Geometry) > 4.0f;
		return FReply::Handled();
	}
	Hover = FRigExecPickerModel::Get().HitAt(Owner->GetPanelIndex(), At);
	return FReply::Unhandled();
}

void
SRigExecPickerCanvas::OnMouseLeave(const FPointerEvent&)
{
	Hover = INDEX_NONE;
}

FReply
SRigExecPickerCanvas::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (!bPressed || Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bPressed = false;
	FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	const int32 PanelIndex = Owner->GetPanelIndex();
	// Shift toggles, Ctrl removes, Alt brings each button's mirror along.
	const int32 Mode = Event.IsControlDown() ? 2 : Event.IsShiftDown() ? 1 : 0;
	TArray<int32> Picked;
	if (bDragging)
	{
		bDragging = false;
		const FVector2f Lo(FMath::Min(PressAt.X, DragAt.X), FMath::Min(PressAt.Y, DragAt.Y));
		const FVector2f Hi(FMath::Max(PressAt.X, DragAt.X), FMath::Max(PressAt.Y, DragAt.Y));
		Picked = Model.Within(PanelIndex, FBox2f(Lo, Hi));
	}
	else
	{
		const int32 Hit = Model.HitAt(PanelIndex, PressAt);
		if (Hit != INDEX_NONE)
		{
			Picked.Add(Hit);
		}
	}
	Model.Pick(PanelIndex, Picked, Mode, Event.IsAltDown(), Event.GetScreenSpacePosition(), SharedThis(this));
	return FReply::Handled().ReleaseMouseCapture();
}

// -- panel ----------------------------------------------------------------

void
SRigExecPicker::Construct(const FArguments&)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SAssignNew(Tabs, SHorizontalBox)
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SRigExecPickerCanvas, this)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock).Text_Lambda([]() { return FText::FromString(FRigExecPickerModel::Get().GetStatus()); })
		]
	];
	Sync();
	RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SRigExecPicker::Refresh));
}

EActiveTimerReturnType
SRigExecPicker::Refresh(double, float)
{
	Sync();
	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Continue;
}

void
SRigExecPicker::Sync()
{
	FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	Model.Bind();
	if (Generation == Model.GetGeneration())
	{
		return;
	}
	Generation = Model.GetGeneration();
	PanelIndex = 0;
	Tabs->ClearChildren();
	const TArray<FRigExecPickerPanel>& Panels = Model.GetPanels();
	for (int32 Index = 0; Index < Panels.Num(); ++Index)
	{
		const FString Label =
			Model.GetPickerCount() > 1 ? Panels[Index].Picker + TEXT(" ") + Panels[Index].Label : Panels[Index].Label;
		Tabs->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SButton)
			.Text(FText::FromString(Label))
			.ButtonColorAndOpacity_Lambda([this, Index]() {
				return Index == PanelIndex ? FLinearColor(0.3f, 0.5f, 0.9f) : FLinearColor(0.25f, 0.25f, 0.25f);
			})
			.OnClicked_Lambda([this, Index]() {
				PanelIndex = Index;
				return FReply::Handled();
			})
		];
	}
}

#undef LOCTEXT_NAMESPACE
