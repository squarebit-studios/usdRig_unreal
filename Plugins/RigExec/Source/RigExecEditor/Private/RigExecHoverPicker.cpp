#include "RigExecHoverPicker.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "RigExecDraw.h"
#include "RigExecPickerModel.h"
#include "SLevelViewport.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SViewport.h"

using namespace RigExecHover;
using RigExecDraw::Disc;
using RigExecDraw::Ring;

namespace
{
TSharedPtr<FRigExecHoverPicker> GHoverPicker;

// A press that travels further than this is a drag, not a click.
constexpr float GDragSlop = 3.0f;
// How often the shared picker model looks for a new character or file.
constexpr double GBindSeconds = 0.1;

const FColor GHandleFill(225, 225, 225);
const FColor GHandleCollapsedFill(70, 70, 70);
const FColor GHandleHoverFill(169, 211, 255);
const FColor GHandleEdge(20, 20, 20);
const FColor GLabelColor(235, 235, 235);
const FColor GLabelShadow(0, 0, 0, 170);

FLinearColor
Faded(FColor Color, float Opacity)
{
	FLinearColor Linear(Color);
	Linear.A *= Opacity;
	return Linear;
}

TArray<TSharedPtr<SLevelViewport>>
LevelViewports()
{
	FLevelEditorModule* Module = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	const TSharedPtr<ILevelEditor> Editor = Module ? Module->GetFirstLevelEditor() : nullptr;
	return Editor ? Editor->GetViewports() : TArray<TSharedPtr<SLevelViewport>>();
}

bool
IsViewportHovered(const TSharedPtr<SLevelViewport>& Viewport)
{
	const TSharedPtr<SViewport> Widget = Viewport ? Viewport->GetViewportWidget().Pin() : nullptr;
	return Widget && Widget->IsHovered();
}
} // namespace

int32
SRigExecHoverOverlay::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
                              FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle&, bool) const
{
	return GHoverPicker ? GHoverPicker->Paint(*this, Geometry, Out, LayerId) : LayerId;
}

void
SRigExecHoverOverlay::Construct(const FArguments&, const TSharedPtr<SLevelViewport>& InViewport)
{
	Viewport = InViewport;
	SetVisibility(EVisibility::HitTestInvisible);
	// Hover, selection and the rig's IK/FK state change every frame without
	// telling Slate.
	ForceVolatile(true);
}

// -- on and off -------------------------------------------------------------

void
FRigExecHoverPicker::Startup()
{
	if (GHoverPicker || !FSlateApplication::IsInitialized())
	{
		return;
	}
	GHoverPicker = MakeShared<FRigExecHoverPicker>();
	GHoverPicker->Layout = FLayout::Load();
	// First in line: a press on a picker button is the picker's before Touch
	// Pose or the viewport see it.
	FSlateApplication::Get().RegisterInputPreProcessor(GHoverPicker, 0);
}

void
FRigExecHoverPicker::Shutdown()
{
	if (!GHoverPicker)
	{
		return;
	}
	const bool bWasEnabled = GHoverPicker->Layout.bEnabled;
	GHoverPicker->Layout.bEnabled = false;
	GHoverPicker->SyncOverlays();
	GHoverPicker->Layout.bEnabled = bWasEnabled;
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(GHoverPicker);
	}
	GHoverPicker.Reset();
}

void
FRigExecHoverPicker::SetEnabled(bool bEnabled)
{
	if (!GHoverPicker || GHoverPicker->Layout.bEnabled == bEnabled)
	{
		return;
	}
	GHoverPicker->Layout.bEnabled = bEnabled;
	GHoverPicker->Layout.Save();
	GHoverPicker->Gesture = FGesture();
	GHoverPicker->Hover = FHit();
	if (bEnabled)
	{
		FRigExecPickerModel::Get().Bind();
		GHoverPicker->SyncTabs();
	}
	GHoverPicker->SyncOverlays();
}

bool
FRigExecHoverPicker::IsEnabled()
{
	return GHoverPicker && GHoverPicker->Layout.bEnabled;
}

bool
FRigExecHoverPicker::IsOver(const FVector2D& ScreenPosition)
{
	if (!IsEnabled())
	{
		return false;
	}
	if (GHoverPicker->Gesture.Kind != EGesture::None)
	{
		return true;
	}
	FVector2f Local;
	const TSharedPtr<SRigExecHoverOverlay> Overlay = GHoverPicker->OverlayAt(ScreenPosition, Local);
	return Overlay &&
	       GHoverPicker->HitAt(Local, FVector2f(Overlay->GetTickSpaceGeometry().GetLocalSize())).Kind != EHitKind::None;
}

TArray<FName>
FRigExecHoverPicker::ControlsAt(const FVector2D& ScreenPosition, ARigExecActor*& OutActor)
{
	OutActor = nullptr;
	if (!IsEnabled() || GHoverPicker->Gesture.Kind != EGesture::None)
	{
		return {};
	}
	FVector2f Local;
	const TSharedPtr<SRigExecHoverOverlay> Overlay = GHoverPicker->OverlayAt(ScreenPosition, Local);
	if (!Overlay)
	{
		return {};
	}
	const FHit Hit = GHoverPicker->HitAt(Local, FVector2f(Overlay->GetTickSpaceGeometry().GetLocalSize()));
	const FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	const FRigExecPickerPanel* Panel =
		Hit.Kind == EHitKind::Button ? Model.GetPanel(GHoverPicker->Tabs[Hit.Tab].Panel) : nullptr;
	if (!Panel)
	{
		return {};
	}
	OutActor = Model.GetActor();
	return Panel->Buttons[Hit.Button].Targets;
}

// -- keeping up ---------------------------------------------------------------

void
FRigExecHoverPicker::SyncOverlays()
{
	const TArray<TSharedPtr<SLevelViewport>> Viewports = Layout.bEnabled ? LevelViewports() : TArray<TSharedPtr<SLevelViewport>>();
	// Off, or its viewport gone (a layout change remakes them): take it off.
	for (int32 I = Overlays.Num() - 1; I >= 0; --I)
	{
		const TSharedPtr<SLevelViewport> Viewport = Overlays[I].Viewport.Pin();
		if (!Viewport || !Viewports.Contains(Viewport))
		{
			if (Viewport)
			{
				Viewport->RemoveOverlayWidget(Overlays[I].Widget.ToSharedRef());
			}
			Overlays.RemoveAt(I);
		}
	}
	for (const TSharedPtr<SLevelViewport>& Viewport : Viewports)
	{
		if (!Viewport || Overlays.ContainsByPredicate([&](const FViewportOverlay& O) { return O.Viewport == Viewport; }))
		{
			continue;
		}
		FViewportOverlay& Overlay = Overlays.AddDefaulted_GetRef();
		Overlay.Viewport = Viewport;
		Overlay.Widget = SNew(SRigExecHoverOverlay, Viewport);
		Viewport->AddOverlayWidget(Overlay.Widget.ToSharedRef());
	}
}

void
FRigExecHoverPicker::SyncTabs()
{
	const FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	if (Generation == Model.GetGeneration())
	{
		return;
	}
	Generation = Model.GetGeneration();
	Tabs.Reset();
	Hover = FHit();
	Gesture = FGesture();
	float ViewHeight = 0.0f;
	for (const FViewportOverlay& Overlay : Overlays)
	{
		ViewHeight = FMath::Max(ViewHeight, float(Overlay.Widget->GetTickSpaceGeometry().GetLocalSize().Y));
	}
	TArray<FString> Keys;
	TMap<FString, float> Fits;
	const TArray<FRigExecPickerPanel>& Panels = Model.GetPanels();
	for (int32 PanelIndex = 0; PanelIndex < Panels.Num(); ++PanelIndex)
	{
		const FRigExecPickerPanel& Panel = Panels[PanelIndex];
		// The group's corner is the box around the buttons it can show, in
		// either half of every IK/FK switch: buttons this rig lacks and
		// backdrops are never drawn here, so they must not push the buttons
		// away from the handle.
		FBox2f Box(ForceInit);
		for (int32 Index : Model.Visible(PanelIndex, true))
		{
			if (!Panel.Buttons[Index].bDecoration)
			{
				Box += Panel.Buttons[Index].Box;
			}
		}
		if (!Box.bIsValid)
		{
			continue;
		}
		FTab& Tab = Tabs.AddDefaulted_GetRef();
		Tab.Panel = PanelIndex;
		Tab.Key = TabKey(Panel.Picker, Panel.Label);
		Tab.Label = Model.GetPickerCount() > 1 ? Panel.Picker + TEXT(" ") + Panel.Label : Panel.Label;
		Tab.Origin = Box.Min;
		Tab.Content = Box.GetSize();
		Keys.Add(Tab.Key);
		Fits.Add(Tab.Key, FitScale(Tab.Content.Y, ViewHeight > 0.0f ? ViewHeight : 800.0f));
	}
	const int32 Before = Layout.Tabs.Num();
	Layout.Ensure(Keys, Fits);
	if (Layout.Tabs.Num() != Before)
	{
		Layout.Save();
	}
}

void
FRigExecHoverPicker::Tick(const float, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor)
{
	if (!Layout.bEnabled)
	{
		if (Overlays.Num() > 0)
		{
			SyncOverlays();
		}
		return;
	}
	// A release that never reached us (outside the editor) ends the gesture.
	if (Gesture.Kind != EGesture::None && !SlateApp.GetPressedMouseButtons().Contains(Gesture.Mouse))
	{
		Gesture = FGesture();
		Layout.Save();
	}
	const double Now = FPlatformTime::Seconds();
	if (Now >= NextBind)
	{
		NextBind = Now + GBindSeconds;
		FRigExecPickerModel::Get().Bind();
		SyncOverlays();
	}
	SyncTabs();
	// The hover, from where the cursor is now: a move event arrives before
	// Slate updates which viewport is hovered, and the picker under a still
	// cursor can change (a tab collapsed, a switch flipped).
	if (Gesture.Kind == EGesture::None)
	{
		FVector2f Local;
		const TSharedPtr<SRigExecHoverOverlay> Overlay =
			SlateApp.GetPressedMouseButtons().Num() == 0 ? OverlayAt(Cursor->GetPosition(), Local) : nullptr;
		SetHover(Overlay ? HitAt(Local, FVector2f(Overlay->GetTickSpaceGeometry().GetLocalSize())) : FHit(), Overlay);
	}
}

// -- where the cursor is ------------------------------------------------------

TSharedPtr<SRigExecHoverOverlay>
FRigExecHoverPicker::OverlayAt(const FVector2D& ScreenPosition, FVector2f& OutLocal) const
{
	for (const FViewportOverlay& Overlay : Overlays)
	{
		if (!IsViewportHovered(Overlay.Viewport.Pin()))
		{
			continue;
		}
		const FGeometry& Geometry = Overlay.Widget->GetTickSpaceGeometry();
		const FVector2f Local = FVector2f(Geometry.AbsoluteToLocal(ScreenPosition));
		const FVector2f Size = FVector2f(Geometry.GetLocalSize());
		if (Local.X >= 0.0f && Local.Y >= 0.0f && Local.X < Size.X && Local.Y < Size.Y)
		{
			OutLocal = Local;
			return Overlay.Widget;
		}
	}
	return nullptr;
}

FRigExecHoverPicker::FHit
FRigExecHoverPicker::HitAt(const FVector2f& Point, const FVector2f& ViewSize) const
{
	FHit Hit;
	if (HitsKnob(ViewSize, Point))
	{
		Hit.Kind = EHitKind::Knob;
		return Hit;
	}
	const FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	for (int32 T = Tabs.Num() - 1; T >= 0; --T)
	{
		const FTabLayout* Tab = Layout.Tabs.Find(Tabs[T].Key);
		if (!Tab)
		{
			continue;
		}
		if (Tab->HitsHandle(Point))
		{
			Hit.Kind = EHitKind::Handle;
			Hit.Tab = T;
			return Hit;
		}
		if (Tab->bCollapsed)
		{
			continue;
		}
		const int32 Button = Model.HitAt(Tabs[T].Panel, Tab->ToPanel(Tabs[T].Origin, Point));
		if (Button != INDEX_NONE)
		{
			Hit.Kind = EHitKind::Button;
			Hit.Tab = T;
			Hit.Button = Button;
			return Hit;
		}
	}
	return Hit;
}

void
FRigExecHoverPicker::SetHover(const FHit& Hit, const TSharedPtr<SRigExecHoverOverlay>& Overlay)
{
	Hover = Hit;
	HoverOverlay = Hit.Kind == EHitKind::None ? nullptr : Overlay;
}

// -- the keyboard -------------------------------------------------------------

bool
FRigExecHoverPicker::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() != EKeys::P)
	{
		return false;
	}
	if (bKeyLatched)
	{
		return true;
	}
	if (InKeyEvent.IsRepeat() || InKeyEvent.GetModifierKeys().AnyModifiersDown())
	{
		return false;
	}
	// Only over a level viewport, only with a character in the level, and
	// never into a text field: anywhere else P keeps its usual meaning.
	if (!LevelViewports().ContainsByPredicate([](const TSharedPtr<SLevelViewport>& V) { return IsViewportHovered(V); }))
	{
		return false;
	}
	const TSharedPtr<SWidget> Focus = SlateApp.GetKeyboardFocusedWidget();
	if (Focus && Focus->GetType().ToString().Contains(TEXT("EditableText")))
	{
		return false;
	}
	FRigExecPickerModel::Get().Bind();
	if (!FRigExecPickerModel::Get().GetActor())
	{
		return false;
	}
	bKeyLatched = true;
	SetEnabled(!IsEnabled());
	return true;
}

bool
FRigExecHoverPicker::HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::P && bKeyLatched)
	{
		bKeyLatched = false;
		return true;
	}
	return false;
}

// -- the mouse ----------------------------------------------------------------

bool
FRigExecHoverPicker::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (!Layout.bEnabled)
	{
		return false;
	}
	if (Gesture.Kind == EGesture::None)
	{
		FVector2f Local;
		const TSharedPtr<SRigExecHoverOverlay> Overlay =
			SlateApp.GetPressedMouseButtons().Num() == 0 ? OverlayAt(MouseEvent.GetScreenSpacePosition(), Local) : nullptr;
		SetHover(Overlay ? HitAt(Local, FVector2f(Overlay->GetTickSpaceGeometry().GetLocalSize())) : FHit(), Overlay);
		return false;
	}
	const TSharedPtr<SRigExecHoverOverlay> Overlay = Gesture.Overlay.Pin();
	if (!Overlay)
	{
		Gesture = FGesture();
		return false;
	}
	const FVector2f Point = FVector2f(Overlay->GetTickSpaceGeometry().AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
	FTabLayout* Tab = Tabs.IsValidIndex(Gesture.Tab) ? Layout.Tabs.Find(Tabs[Gesture.Tab].Key) : nullptr;
	switch (Gesture.Kind)
	{
	case EGesture::Knob:
		Layout.Faded(Gesture.StartValue, Point.X - Gesture.Start.X);
		break;
	case EGesture::Move:
		if (Tab)
		{
			Tab->Moved(Point - Gesture.Last);
		}
		Gesture.Last = Point;
		break;
	case EGesture::Scale:
		if (Tab)
		{
			Tab->Scaled(Gesture.StartValue, Point - Gesture.Start);
		}
		break;
	case EGesture::Fade:
		if (Tab)
		{
			Tab->Faded(Gesture.StartValue, Point.X - Gesture.Start.X);
		}
		break;
	case EGesture::Pick:
		Gesture.bBand = Gesture.bBand || FVector2f::Distance(Point, Gesture.Start) > GDragSlop;
		BandEnd = Point;
		break;
	default:
		break;
	}
	return true;
}

bool
FRigExecHoverPicker::HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent& MouseEvent)
{
	return Press(MouseEvent, false);
}

bool
FRigExecHoverPicker::HandleMouseButtonDoubleClickEvent(FSlateApplication&, const FPointerEvent& MouseEvent)
{
	return Press(MouseEvent, true);
}

bool
FRigExecHoverPicker::Press(const FPointerEvent& MouseEvent, bool bDouble)
{
	if (!Layout.bEnabled)
	{
		return false;
	}
	if (Gesture.Kind != EGesture::None)
	{
		return true;
	}
	const FKey Mouse = MouseEvent.GetEffectingButton();
	// Alt is the camera's (orbit, dolly, pan) everywhere except on a picker
	// button, where it is the docked picker's mirror modifier.
	if (MouseEvent.IsAltDown() && Mouse != EKeys::LeftMouseButton)
	{
		return false;
	}
	FVector2f Point;
	const TSharedPtr<SRigExecHoverOverlay> Overlay = OverlayAt(MouseEvent.GetScreenSpacePosition(), Point);
	if (!Overlay)
	{
		return false;
	}
	const FHit Hit = HitAt(Point, FVector2f(Overlay->GetTickSpaceGeometry().GetLocalSize()));
	if (Hit.Kind == EHitKind::None || (MouseEvent.IsAltDown() && Hit.Kind != EHitKind::Button))
	{
		return false;
	}
	FTabLayout* Tab = Hit.Tab != INDEX_NONE ? Layout.Tabs.Find(Tabs[Hit.Tab].Key) : nullptr;
	if (Mouse == EKeys::LeftMouseButton && bDouble && Hit.Kind != EHitKind::Button)
	{
		if (Hit.Kind == EHitKind::Handle && Tab)
		{
			Tab->bCollapsed = !Tab->bCollapsed;
		}
		else if (Hit.Kind == EHitKind::Knob)
		{
			Layout.Opacity = 1.0f;
		}
		Layout.Save();
		SwallowUp = Mouse;
		return true;
	}
	FGesture Next;
	Next.Mouse = Mouse;
	Next.Tab = Hit.Tab;
	Next.Overlay = Overlay;
	Next.Start = Next.Last = Point;
	if (Mouse == EKeys::LeftMouseButton)
	{
		if (Hit.Kind == EHitKind::Knob)
		{
			Next.Kind = EGesture::Knob;
			Next.StartValue = Layout.Opacity;
		}
		else if (Hit.Kind == EHitKind::Handle)
		{
			Next.Kind = MouseEvent.IsShiftDown() ? EGesture::Scale : EGesture::Move;
			Next.StartValue = Tab ? Tab->Scale : 1.0f;
		}
		else
		{
			Next.Kind = EGesture::Pick;
			Next.PanelStart = Tab ? Tab->ToPanel(Tabs[Hit.Tab].Origin, Point) : FVector2f::ZeroVector;
			// Shift toggles, Ctrl removes, Alt brings each button's mirror.
			Next.Mode = MouseEvent.IsControlDown() ? 2 : MouseEvent.IsShiftDown() ? 1 : 0;
			Next.bMirror = MouseEvent.IsAltDown();
		}
	}
	else if (Mouse == EKeys::MiddleMouseButton)
	{
		if (Hit.Kind == EHitKind::Knob)
		{
			Next.Kind = EGesture::Knob;
			Next.StartValue = Layout.Opacity;
		}
		else
		{
			Next.Kind = EGesture::Fade;
			Next.StartValue = Tab ? Tab->Opacity : 1.0f;
		}
	}
	else
	{
		// Right-click and anything else stay the viewport's.
		return false;
	}
	Gesture = Next;
	BandEnd = Point;
	return true;
}

bool
FRigExecHoverPicker::HandleMouseButtonUpEvent(FSlateApplication&, const FPointerEvent& MouseEvent)
{
	const FKey Mouse = MouseEvent.GetEffectingButton();
	if (Gesture.Kind != EGesture::None)
	{
		if (Mouse != Gesture.Mouse)
		{
			return true;
		}
		if (Gesture.Kind == EGesture::Pick)
		{
			FinishPick(MouseEvent.GetScreenSpacePosition());
		}
		else
		{
			Layout.Save();
		}
		Gesture = FGesture();
		return true;
	}
	if (SwallowUp.IsValid() && Mouse == SwallowUp)
	{
		SwallowUp = FKey();
		return true;
	}
	return false;
}

void
FRigExecHoverPicker::FinishPick(const FVector2D& ScreenAt)
{
	const TSharedPtr<SRigExecHoverOverlay> Overlay = Gesture.Overlay.Pin();
	const FTabLayout* Tab = Tabs.IsValidIndex(Gesture.Tab) ? Layout.Tabs.Find(Tabs[Gesture.Tab].Key) : nullptr;
	if (!Overlay || !Tab)
	{
		return;
	}
	FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	const FTab& Shown = Tabs[Gesture.Tab];
	TArray<int32> Picked;
	if (Gesture.bBand)
	{
		const FVector2f A = Tab->ToPanel(Shown.Origin, Gesture.Start);
		const FVector2f B = Tab->ToPanel(Shown.Origin, BandEnd);
		Picked = Model.Within(Shown.Panel, FBox2f(FVector2f(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)),
		                                          FVector2f(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y))));
	}
	else
	{
		const int32 Hit = Model.HitAt(Shown.Panel, Gesture.PanelStart);
		if (Hit != INDEX_NONE)
		{
			Picked.Add(Hit);
		}
	}
	// A band over nothing leaves the selection alone.
	if (Picked.Num() > 0)
	{
		Model.Pick(Shown.Panel, Picked, Gesture.Mode, Gesture.bMirror, ScreenAt, Overlay.ToSharedRef());
	}
}

// -- painting -----------------------------------------------------------------

int32
FRigExecHoverPicker::Paint(const SRigExecHoverOverlay& Overlay, const FGeometry& Geometry,
                           FSlateWindowElementList& Out, int32 LayerId) const
{
	if (!Layout.bEnabled)
	{
		return LayerId;
	}
	const FRigExecPickerModel& Model = FRigExecPickerModel::Get();
	const bool bHoverHere = HoverOverlay.Pin().Get() == &Overlay;
	const bool bGestureHere = Gesture.Overlay.Pin().Get() == &Overlay;
	for (int32 T = 0; T < Tabs.Num(); ++T)
	{
		const FTab& Shown = Tabs[T];
		const FTabLayout* Tab = Layout.Tabs.Find(Shown.Key);
		const FRigExecPickerPanel* Panel = Model.GetPanel(Shown.Panel);
		if (!Tab || !Panel)
		{
			continue;
		}
		const float Opacity = Layout.TabOpacity(Shown.Key);
		if (!Tab->bCollapsed)
		{
			const FVector2f Offset = Tab->ButtonsCorner() - Shown.Origin * Tab->Scale;
			for (int32 Index : Model.Visible(Shown.Panel))
			{
				const FRigExecPickerButton& Button = Panel->Buttons[Index];
				if (Button.bDecoration)
				{
					continue;
				}
				const bool bHovered = bHoverHere && Hover.Kind == EHitKind::Button && Hover.Tab == T && Hover.Button == Index;
				LayerId += 4;
				Model.PaintButton(Button, bHovered, Geometry, Offset, Tab->Scale, Opacity, Out, LayerId);
			}
		}
		const bool bHandleHovered = bHoverHere && Hover.Kind == EHitKind::Handle && Hover.Tab == T;
		const bool bActive = bGestureHere && Gesture.Tab == T &&
		                     (Gesture.Kind == EGesture::Move || Gesture.Kind == EGesture::Scale ||
		                      Gesture.Kind == EGesture::Fade);
		LayerId += 4;
		PaintHandle(Out, LayerId, Geometry, *Tab, bHandleHovered || bActive, Opacity);
		if (bHandleHovered || bActive)
		{
			FString Text = Shown.Label;
			if (bActive && Gesture.Kind == EGesture::Scale)
			{
				Text += FString::Printf(TEXT("  %d%%"), FMath::RoundToInt(Tab->Scale * 100.0f));
			}
			else if (bActive && Gesture.Kind == EGesture::Fade)
			{
				Text += FString::Printf(TEXT("  opacity %d%%"), FMath::RoundToInt(Tab->Opacity * 100.0f));
			}
			PaintText(Out, LayerId + 2, Geometry, FVector2f(Tab->X + HandleRadius + 6.0f, Tab->Y), Text, false, Opacity);
		}
	}
	if (bGestureHere && Gesture.Kind == EGesture::Pick && Gesture.bBand)
	{
		const FVector2f Lo(FMath::Min(Gesture.Start.X, BandEnd.X), FMath::Min(Gesture.Start.Y, BandEnd.Y));
		const FVector2f Hi(FMath::Max(Gesture.Start.X, BandEnd.X), FMath::Max(Gesture.Start.Y, BandEnd.Y));
		FSlateDrawElement::MakeBox(Out, ++LayerId, Geometry.ToPaintGeometry(Hi - Lo, FSlateLayoutTransform(Lo)),
		                           FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None,
		                           FLinearColor(FColor(120, 200, 255, 40)));
		TArray<FVector2f> Band = {Lo, FVector2f(Hi.X, Lo.Y), Hi, FVector2f(Lo.X, Hi.Y), Lo};
		FSlateDrawElement::MakeLines(Out, ++LayerId, Geometry.ToPaintGeometry(), Band, ESlateDrawEffect::None,
		                             FLinearColor(FColor(150, 215, 255)), true, 1.0f);
	}
	LayerId += 4;
	PaintKnob(Out, LayerId, Geometry,
	          (bHoverHere && Hover.Kind == EHitKind::Knob) || (bGestureHere && Gesture.Kind == EGesture::Knob));
	return LayerId + 4;
}

void
FRigExecHoverPicker::PaintHandle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
                                 const FTabLayout& Tab, bool bHovered, float Opacity) const
{
	const FVector2f Centre(Tab.X, Tab.Y);
	const FColor Fill = bHovered ? GHandleHoverFill : Tab.bCollapsed ? GHandleCollapsedFill : GHandleFill;
	Disc(Out, Layer, Geometry, Centre, HandleRadius, Faded(Fill, Opacity));
	Ring(Out, Layer + 1, Geometry, Centre, HandleRadius, Faded(GHandleEdge, Opacity), 1.5f);
	if (Tab.bCollapsed)
	{
		// A collapsed tab reads as a ring with a dot in it.
		Disc(Out, Layer + 2, Geometry, Centre, HandleRadius * 0.35f, Faded(GHandleFill, Opacity));
	}
}

void
FRigExecHoverPicker::PaintKnob(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
                               bool bHovered) const
{
	const FVector2f Centre = KnobCentre(FVector2f(Geometry.GetLocalSize()));
	const FColor Edge = bHovered ? GHandleHoverFill : GHandleFill;
	Disc(Out, Layer, Geometry, Centre, KnobRadius, FLinearColor(FColor(40, 40, 40, 200)));
	Ring(Out, Layer + 1, Geometry, Centre, KnobRadius, FLinearColor(Edge), 1.5f);
	// The overall opacity as a filled wedge, full at 100%.
	Disc(Out, Layer + 2, Geometry, Centre, KnobRadius - 2.0f, FLinearColor(Edge), Layout.Opacity);
	if (bHovered)
	{
		PaintText(Out, Layer + 3, Geometry, FVector2f(Centre.X - KnobRadius - 6.0f, Centre.Y),
		          FString::Printf(TEXT("picker opacity %d%%"), FMath::RoundToInt(Layout.Opacity * 100.0f)), true, 1.0f);
	}
}

void
FRigExecHoverPicker::PaintText(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
                               const FVector2f& At, const FString& Text, bool bAlignRight, float Opacity) const
{
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 8);
	const FVector2f Extent =
		FVector2f(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font));
	const FVector2f Corner(bAlignRight ? At.X - Extent.X : At.X, At.Y - Extent.Y * 0.5f);
	FSlateDrawElement::MakeText(Out, Layer, Geometry.ToPaintGeometry(Extent, FSlateLayoutTransform(Corner + FVector2f(1.0f, 1.0f))),
	                            Text, Font, ESlateDrawEffect::None, Faded(GLabelShadow, Opacity));
	FSlateDrawElement::MakeText(Out, Layer + 1, Geometry.ToPaintGeometry(Extent, FSlateLayoutTransform(Corner)), Text,
	                            Font, ESlateDrawEffect::None, Faded(GLabelColor, Opacity));
}
