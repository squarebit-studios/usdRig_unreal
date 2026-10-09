#include "RigExecTouchPose.h"

#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "ControlRigGizmoActor.h"
#include "Editor.h"
#include "EditorGizmos/EditorTransformGizmoUtil.h"
#include "EditorGizmos/TransformGizmo.h"
#include "EditorInteractiveGizmoManager.h"
#include "EditorModeManager.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorViewport.h"
#include "RigExecActor.h"
#include "RigExecComponent.h"
#include "RigExecHoverPicker.h"
#include "ScopedTransaction.h"
#include "SceneView.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "UnrealWidget.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

#define LOCTEXT_NAMESPACE "RigExecTouchPose"

namespace
{
TSharedPtr<FRigExecTouchPose> GTouchPose;

// How long after the rig last posed Touch Pose stays off: playback, a key
// change or a typed value re-pose it every frame, and the highlight returns
// once it has been still this long.
constexpr double GSettleSeconds = 0.35;
// The square (pixels each way) searched for a manipulator around a click.
constexpr int32 GManipulatorSlop = 6;

// Whether a manipulator is at or near `Pixel`: the transform widget (hovered,
// or under the cursor) or a Control Rig control shape. A manipulator always
// wins the click over the skin.
bool
ManipulatorNear(FLevelEditorViewportClient* Client, const FIntPoint& Pixel)
{
	if (Client->GetCurrentWidgetAxis() != EAxisList::None)
	{
		return true;
	}
	const FIntPoint Size = Client->Viewport->GetSizeXY();
	const FIntRect Rect(FMath::Max(Pixel.X - GManipulatorSlop, 0), FMath::Max(Pixel.Y - GManipulatorSlop, 0),
	                    FMath::Min(Pixel.X + GManipulatorSlop + 1, Size.X), FMath::Min(Pixel.Y + GManipulatorSlop + 1, Size.Y));
	if (Rect.Area() <= 0)
	{
		return false;
	}
	TArray<HHitProxy*> Proxies;
	Client->Viewport->GetHitProxyMap(Rect, Proxies);
	for (HHitProxy* Proxy : Proxies)
	{
		if (!Proxy)
		{
			continue;
		}
		if (Proxy->IsA(HWidgetAxis::StaticGetType()))
		{
			return true;
		}
		const HActor* ActorProxy = HitProxyCast<HActor>(Proxy);
		if (ActorProxy && ActorProxy->Actor && ActorProxy->Actor->IsA<AControlRigShapeActor>())
		{
			return true;
		}
	}
	return false;
}

// Whether the editor's transform gizmo (the Interactive Tools Framework
// one, when "new TRS gizmos" are on) would take a press along this ray. It
// does its own ray test rather than drawing hit proxies, so the hit-proxy
// scan above cannot see it; this asks it the question its input router asks.
bool
TransformGizmoWants(const FVector& Origin, const FVector& Direction, const FVector2D& ScreenPixel)
{
	if (!UEditorInteractiveGizmoManager::UsesNewTRSGizmos())
	{
		return false;
	}
	UModeManagerInteractiveToolsContext* Context = GLevelEditorModeTools().GetInteractiveToolsContext();
	UTransformGizmo* Gizmo =
		Context ? UE::EditorTransformGizmoUtil::FindDefaultTransformGizmo(Context->ToolManager) : nullptr;
	return Gizmo && Gizmo->CanBeginClickDragSequence(FInputDeviceRay(FRay(Origin, Direction, true), ScreenPixel)).bHit;
}

// Whether the character is being worked: a left-button drag anywhere (a
// gizmo, a slider, the timeline) or a pose within the settle time.
bool
IsBusy(const ARigExecActor* Actor, bool bButtonHeld)
{
	return bButtonHeld || (Actor->Rig && FPlatformTime::Seconds() - Actor->Rig->GetLastPoseTime() < GSettleSeconds);
}

UControlRig*
ControlRigOf(const ARigExecActor* Actor)
{
	// Sequencer swaps in its own Control Rig instance; the component hands
	// out whichever is live.
	return Actor && Actor->Controls ? Actor->Controls->GetControlRig() : nullptr;
}
} // namespace

void
FRigExecTouchPose::SetEnabled(bool bEnabled)
{
	if (bEnabled == IsEnabled() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	if (bEnabled)
	{
		GTouchPose = MakeShared<FRigExecTouchPose>();
		FSlateApplication::Get().RegisterInputPreProcessor(GTouchPose);
	}
	else
	{
		GTouchPose->ClearAll();
		FSlateApplication::Get().UnregisterInputPreProcessor(GTouchPose);
		GTouchPose.Reset();
	}
}

bool
FRigExecTouchPose::IsEnabled()
{
	return GTouchPose.IsValid();
}

void
FRigExecTouchPose::Shutdown()
{
	if (GTouchPose && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(GTouchPose);
	}
	GTouchPose.Reset();
}

FName
FRigExecTouchPose::ControlAt(const FVector2D& ScreenPosition, ARigExecActor*& OutActor)
{
	OutActor = nullptr;
	if (!GTouchPose)
	{
		return NAME_None;
	}
	const FHit Hit = GTouchPose->HitAt(ScreenPosition);
	ARigExecActor* Actor = Hit.Actor.Get();
	if (!Actor || Hit.Region < 0 || !Hit.Client || Actor->Rig->IsTouchSuspended())
	{
		return NAME_None;
	}
	// The skin must be what is under the cursor, as for a click.
	if (HHitProxy* Proxy = Hit.Client->Viewport->GetHitProxy(Hit.Pixel.X, Hit.Pixel.Y))
	{
		const HActor* ActorProxy = HitProxyCast<HActor>(Proxy);
		if (!ActorProxy || ActorProxy->Actor != Actor)
		{
			return NAME_None;
		}
	}
	OutActor = Actor;
	return Actor->Rig->GetTouchRegionControl(Hit.Region);
}

FRigExecTouchPose::FHit
FRigExecTouchPose::HitAt(const FVector2D& ScreenPosition) const
{
	FHit Hit;
	if (!GEditor)
	{
		return Hit;
	}
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		FSceneViewport* Viewport = Client ? static_cast<FSceneViewport*>(Client->Viewport) : nullptr;
		const TSharedPtr<SViewport> Widget = Viewport ? Viewport->GetViewportWidget().Pin() : nullptr;
		if (!Widget || !Widget->IsHovered() || !Client->GetWorld())
		{
			continue;
		}
		// Screen to the viewport's pixels.
		const FGeometry& Geometry = Viewport->GetCachedGeometry();
		const FVector2D Local = Geometry.AbsoluteToLocal(ScreenPosition);
		const FVector2D Size = Geometry.GetLocalSize();
		const FIntPoint Pixels = Viewport->GetSizeXY();
		if (Size.X <= 0.0 || Size.Y <= 0.0 || Local.X < 0.0 || Local.Y < 0.0 || Local.X >= Size.X ||
		    Local.Y >= Size.Y)
		{
			continue;
		}
		const FVector2D Pixel(Local.X * Pixels.X / Size.X, Local.Y * Pixels.Y / Size.Y);

		FSceneViewFamilyContext Family(
			FSceneViewFamily::ConstructionValues(Viewport, Client->GetScene(), Client->EngineShowFlags)
				.SetRealtimeUpdate(Client->IsRealtime()));
		const FSceneView* View = Client->CalcSceneView(&Family);
		FVector Origin, Direction;
		View->DeprojectFVector2D(Pixel, Origin, Direction);

		double Best = TNumericLimits<double>::Max();
		for (TActorIterator<ARigExecActor> It(Client->GetWorld()); It; ++It)
		{
			if (!It->Rig || It->Rig->GetTouchRegionCount() == 0)
			{
				continue;
			}
			double Distance = 0.0;
			const int32 Region = It->Rig->PickTouchRegion(Origin, Direction, Distance);
			if (Distance > 0.0 && Distance < Best)
			{
				Best = Distance;
				Hit.Actor = *It;
				Hit.Region = Region;
			}
		}
		Hit.Client = Client;
		Hit.Pixel = FIntPoint(FMath::FloorToInt(Pixel.X), FMath::FloorToInt(Pixel.Y));
		Hit.Origin = Origin;
		Hit.Direction = Direction;
		Hit.ScreenPixel = Pixel;
		return Hit;
	}
	return Hit;
}

void
FRigExecTouchPose::Highlight(const FHit& Hover) const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	for (TActorIterator<ARigExecActor> It(World); World && It; ++It)
	{
		URigExecComponent* Rig = It->Rig;
		if (!Rig || Rig->GetTouchRegionCount() == 0)
		{
			continue;
		}
		// The selected controls' regions; the last control selected that
		// has one is the lead.
		TArray<int32> Selected;
		int32 Lead = -1;
		if (const UControlRig* ControlRig = ControlRigOf(*It))
		{
			for (const FName& Control : ControlRig->CurrentControlSelection())
			{
				const TArray<int32> Regions = Rig->GetTouchRegionsOf({Control});
				if (Regions.Num() > 0)
				{
					if (Lead >= 0)
					{
						Selected.Add(Lead);
					}
					Lead = Regions[0];
					Selected.Append(Regions.GetData() + 1, Regions.Num() - 1);
				}
			}
		}
		const int32 Hovered = Hover.Actor.Get() == *It ? Hover.Region : -1;
		Rig->SetTouchHighlight(Hovered, Lead, Selected);
	}
}

void
FRigExecTouchPose::ClearAll() const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	for (TActorIterator<ARigExecActor> It(World); World && It; ++It)
	{
		if (It->Rig)
		{
			It->Rig->SetTouchHighlight(-1, -1, {});
		}
	}
}

void
FRigExecTouchPose::Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor)
{
	// Off while the rig is worked, back on when it settles. Touch Pose's own
	// click holds the button too, and does not count.
	const bool bButtonHeld = !bSwallowUp && SlateApp.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton);
	bool bAnyIdle = false;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	for (TActorIterator<ARigExecActor> It(World); World && It; ++It)
	{
		if (It->Rig)
		{
			const bool bBusy = IsBusy(*It, bButtonHeld);
			It->Rig->SetTouchSuspended(bBusy);
			bAnyIdle |= !bBusy;
		}
	}
	FHit Hover;
	if (bAnyIdle && SlateApp.GetPressedMouseButtons().Num() == 0)
	{
		// The hover picker's buttons sit over the skin and win.
		if (!FRigExecHoverPicker::IsOver(Cursor->GetPosition()))
		{
			Hover = HitAt(Cursor->GetPosition());
		}
		// Over the transform gizmo the gizmo is what a click takes.
		if (Hover.Client && (Hover.Client->GetCurrentWidgetAxis() != EAxisList::None ||
		                     TransformGizmoWants(Hover.Origin, Hover.Direction, Hover.ScreenPixel)))
		{
			Hover = FHit();
		}
	}
	Highlight(Hover);
}

bool
FRigExecTouchPose::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || MouseEvent.IsAltDown() ||
	    FRigExecHoverPicker::IsOver(MouseEvent.GetScreenSpacePosition()))
	{
		return false;
	}
	const FHit Hit = HitAt(MouseEvent.GetScreenSpacePosition());
	ARigExecActor* Actor = Hit.Actor.Get();
	if (!Actor || Hit.Region < 0 || !Hit.Client)
	{
		return false;
	}
	// Nothing while the rig is being worked, and a manipulator at or near
	// the cursor always wins.
	if (Actor->Rig->IsTouchSuspended() || TransformGizmoWants(Hit.Origin, Hit.Direction, Hit.ScreenPixel) ||
	    ManipulatorNear(Hit.Client, Hit.Pixel))
	{
		return false;
	}
	// The skin must be what is under the cursor: another object in front
	// keeps the click.
	if (HHitProxy* Proxy = Hit.Client->Viewport->GetHitProxy(Hit.Pixel.X, Hit.Pixel.Y))
	{
		const HActor* ActorProxy = HitProxyCast<HActor>(Proxy);
		if (!ActorProxy || ActorProxy->Actor != Actor)
		{
			return false;
		}
	}
	UControlRig* ControlRig = ControlRigOf(Actor);
	const FName Control = Actor->Rig->GetTouchRegionControl(Hit.Region);
	if (!ControlRig || Control.IsNone())
	{
		return false;
	}
	{
		// Shift toggles, Ctrl removes, a plain click replaces: the picker's
		// rules.
		const FScopedTransaction Transaction(LOCTEXT("TouchSelect", "Touch Select Control"));
		if (MouseEvent.IsControlDown())
		{
			ControlRig->SelectControl(Control, false, true);
		}
		else if (MouseEvent.IsShiftDown())
		{
			ControlRig->SelectControl(Control, !ControlRig->IsControlSelected(Control), true);
		}
		else
		{
			ControlRig->ClearControlSelection(true);
			ControlRig->SelectControl(Control, true, true);
		}
	}
	bSwallowUp = true;
	return true;
}

bool
FRigExecTouchPose::HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	// The viewport never saw the press, so it does not get the release.
	if (bSwallowUp && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bSwallowUp = false;
		return true;
	}
	return false;
}

#undef LOCTEXT_NAMESPACE
