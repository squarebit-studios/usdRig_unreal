#include "RigExecMarkingMenu.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "ControlRigGizmoActor.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Modules/ModuleManager.h"
#include "RigExecActor.h"
#include "RigExecComponent.h"
#include "RigExecDraw.h"
#include "RigExecHoverPicker.h"
#include "RigExecMarkingMenuModel.h"
#include "RigExecTouchPose.h"
#include "Rigs/RigHierarchy.h"
#include "SLevelViewport.h"
#include "ScopedTransaction.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "RigExecMarkingMenu"

using namespace RigExecMarking;

namespace
{
TSharedPtr<FRigExecMarkingMenu> GMarkingMenu;

// The search square (pixels each way) for a control shape under a click.
constexpr int32 GShapeSlop = 6;

// The look: dark rounded pills with a light border, lit blue under the
// cursor.
const FLinearColor GFill(FColor(73, 73, 73));
const FLinearColor GBorder(FColor(133, 133, 133));
const FLinearColor GText(FColor(255, 255, 255));
const FLinearColor GLitFill(FColor(109, 160, 177));
const FLinearColor GLitBorder(FColor(172, 218, 255));
const FLinearColor GLitText(FColor(0, 0, 0));
const FLinearColor GDisabledText(FColor(134, 134, 134));
const FLinearColor GCheck(FColor(129, 212, 250));
const FLinearColor GGuide(FColor(128, 128, 128));
constexpr float GCornerRadius = 7.0f;
constexpr float GBorderWidth = 2.0f;

double
NowMs()
{
	return FPlatformTime::Seconds() * 1000.0;
}

void
Notify(const FText& Text)
{
	UE_LOG(LogTemp, Display, TEXT("RigExec marking menu: %s"), *Text.ToString());
	FNotificationInfo Info(Text);
	Info.ExpireDuration = 2.5f;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);
}

TArray<TSharedPtr<SLevelViewport>>
LevelViewports()
{
	FLevelEditorModule* Module = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	const TSharedPtr<ILevelEditor> Editor = Module ? Module->GetFirstLevelEditor() : nullptr;
	return Editor ? Editor->GetViewports() : TArray<TSharedPtr<SLevelViewport>>();
}

UControlRig*
ControlRigOf(const ARigExecActor* Actor)
{
	return Actor && Actor->Controls ? Actor->Controls->GetControlRig() : nullptr;
}

// The name of the control on the other side, or empty for a name on the
// centre line: the first side marker found is swapped, as usdview's
// CounterpartName (L_Arm -> R_Arm, eye_l_def -> eye_r_def).
FString
CounterpartName(const FString& Name)
{
	struct FSwap
	{
		const TCHAR* From;
		const TCHAR* To;
		int32 Where;  // 0 prefix, 1 infix, 2 suffix
	};
	static const FSwap Swaps[] = {
		{TEXT("L_"), TEXT("R_"), 0}, {TEXT("R_"), TEXT("L_"), 0}, {TEXT("l_"), TEXT("r_"), 0}, {TEXT("r_"), TEXT("l_"), 0},
		{TEXT("_L_"), TEXT("_R_"), 1}, {TEXT("_R_"), TEXT("_L_"), 1}, {TEXT("_l_"), TEXT("_r_"), 1}, {TEXT("_r_"), TEXT("_l_"), 1},
		{TEXT("_L"), TEXT("_R"), 2}, {TEXT("_R"), TEXT("_L"), 2}, {TEXT("_l"), TEXT("_r"), 2}, {TEXT("_r"), TEXT("_l"), 2},
		{TEXT("Left"), TEXT("Right"), 0}, {TEXT("Right"), TEXT("Left"), 0}, {TEXT("left"), TEXT("right"), 0}, {TEXT("right"), TEXT("left"), 0},
	};
	for (const FSwap& Swap : Swaps)
	{
		const int32 Length = FCString::Strlen(Swap.From);
		if (Swap.Where == 0 && Name.StartsWith(Swap.From, ESearchCase::CaseSensitive))
		{
			return FString(Swap.To) + Name.RightChop(Length);
		}
		if (Swap.Where == 2 && Name.EndsWith(Swap.From, ESearchCase::CaseSensitive))
		{
			return Name.LeftChop(Length) + Swap.To;
		}
		if (Swap.Where == 1)
		{
			const int32 At = Name.Find(Swap.From, ESearchCase::CaseSensitive);
			if (At != INDEX_NONE)
			{
				return Name.Left(At) + Swap.To + Name.RightChop(At + Length);
			}
		}
	}
	return FString();
}

/** markingMenuModel's context for the controls the menu acts on. */
class FSelectionContext : public IContext
{
public:
	FSelectionContext(ARigExecActor* InActor, const TArray<FName>& InControls) : Actor(InActor), Controls(InControls) {}

	URigExecComponent* Rig() const { return Actor.IsValid() ? Actor->Rig.Get() : nullptr; }
	UControlRig* ControlRig() const { return ControlRigOf(Actor.Get()); }

	/** The switch of the first limb the controls belong to. */
	FString LimbSwitch() const
	{
		if (const URigExecComponent* Component = Rig())
		{
			for (const FName& Control : Controls)
			{
				const FString Switch = Component->FindLimbSwitchFor(Control);
				if (!Switch.IsEmpty())
				{
					return Switch;
				}
			}
		}
		return FString();
	}

	/** The lead control's (the last selected) space channel and labels. */
	bool Space(FName& OutChannel, TArray<FString>& OutLabels) const
	{
		const URigExecComponent* Component = Rig();
		return Component && Controls.Num() > 0 && Component->FindSpaceChannel(Controls.Last(), OutChannel, OutLabels);
	}

	virtual bool Test(const FString& Name) const override
	{
		if (Name == TEXT("control"))
		{
			return Controls.Num() > 0;
		}
		if (Name == TEXT("limb"))
		{
			return !LimbSwitch().IsEmpty();
		}
		if (Name == TEXT("hasSpaces"))
		{
			FName Channel;
			TArray<FString> Labels;
			return Space(Channel, Labels);
		}
		return false;
	}

	virtual TOptional<double> Read(const FString& Name) const override
	{
		if (Name == TEXT("limbIsIk"))
		{
			const FString Switch = LimbSwitch();
			return !Switch.IsEmpty() && Rig()->IsLimbIk(Switch) ? 1.0 : 0.0;
		}
		if (Name == TEXT("space"))
		{
			FName Channel;
			TArray<FString> Labels;
			UControlRig* Controls_ = ControlRig();
			if (!Controls_ || !Space(Channel, Labels))
			{
				return {};
			}
			return double(FMath::RoundToInt(
				Controls_->GetHierarchy()->GetControlValue(FRigElementKey(Channel, ERigElementType::Control)).Get<float>()));
		}
		return {};
	}

	virtual FString Label(const FString& ItemId, const FString& Default) const override
	{
		if (ItemId == TEXT("selection.fkik"))
		{
			const FString Switch = LimbSwitch();
			if (!Switch.IsEmpty())
			{
				return Rig()->IsLimbIk(Switch) ? TEXT("Switch to FK") : TEXT("Switch to IK");
			}
		}
		return Default;
	}

	virtual TArray<FItem> Children(const FString& Name) const override
	{
		TArray<FItem> Out;
		FName Channel;
		TArray<FString> Labels;
		if (Name == TEXT("spaces") && Space(Channel, Labels))
		{
			for (int32 I = 0; I < Labels.Num(); ++I)
			{
				FItem& Item = Out.AddDefaulted_GetRef();
				Item.Id = FString::Printf(TEXT("selection.space.%d"), I);
				Item.Label = Labels[I];
				Item.Kind = EKind::Radio;
				Item.Read = TEXT("space");
				Item.Value = I;
				Item.Write = TEXT("SetSpace");
			}
		}
		return Out;
	}

	TWeakObjectPtr<ARigExecActor> Actor;
	TArray<FName> Controls;
};
} // namespace

// -- the controller -------------------------------------------------------------

/** Owns one right-click gesture: where it is, what is drawn, and what the
 * confirmed item does. */
class FRigExecMarkingMenuController
{
public:
	FRigExecMarkingMenuController()
	{
		Font = FCoreStyle::GetDefaultFontStyle("Regular", 10);
		Base = float(Measure().Measure(TEXT("Ag"), Font).Y);
		const float R = RadiusPx;
		const float Ry = R * 0.7f;
		const float Dx = R * 0.8f;
		const float Dy = R * 0.45f * 0.7f + Base * 1.2f;
		// An ellipse rather than a circle: wide labels need the room left
		// and right more than above and below.
		Anchors[int32(EDir::N)] = FVector2f(0.0f, -Ry);
		Anchors[int32(EDir::S)] = FVector2f(0.0f, Ry);
		Anchors[int32(EDir::E)] = FVector2f(R, 0.0f);
		Anchors[int32(EDir::W)] = FVector2f(-R, 0.0f);
		Anchors[int32(EDir::NE)] = FVector2f(Dx, -Dy);
		Anchors[int32(EDir::NW)] = FVector2f(-Dx, -Dy);
		Anchors[int32(EDir::SE)] = FVector2f(Dx, Dy);
		Anchors[int32(EDir::SW)] = FVector2f(-Dx, Dy);
		PillBrush = MakeUnique<FSlateRoundedBoxBrush>(GFill, GCornerRadius, GBorder, GBorderWidth);
		LitPillBrush = MakeUnique<FSlateRoundedBoxBrush>(GLitFill, GCornerRadius, GLitBorder, GBorderWidth);
		DisabledPillBrush = MakeUnique<FSlateRoundedBoxBrush>(FLinearColor::Transparent, GCornerRadius);
	}

	~FRigExecMarkingMenuController() { Close(); }

	bool IsOpen() const { return Gesture.IsValid(); }

	// -- input --------------------------------------------------------------------

	bool Press(const FPointerEvent& Event)
	{
		if (IsOpen())
		{
			const FVector2f Point = ToLocal(Event.GetScreenSpacePosition());
			UE_LOG(LogTemp, Log, TEXT("RigExec marking menu: press while open %s"), *Event.GetEffectingButton().ToString());
			LastPoint = Point;
			if (Gesture->GetState() == EState::Click)
			{
				ClickAt(Point);
			}
			else
			{
				Handle(Gesture->Press(Point, NowMs()));
			}
			return true;
		}
		if (Event.GetEffectingButton() != EKeys::RightMouseButton)
		{
			return false;
		}
		ARigExecActor* Actor = nullptr;
		TSharedPtr<SLevelViewport> Under;
		const TArray<FName> Controls = Targets(Event.GetScreenSpacePosition(), Event.IsAltDown(), Actor, Under);
		if (Controls.Num() == 0)
		{
			return false;
		}
		Open(Under, Actor, Controls, Event.GetScreenSpacePosition());
		bPressed = true;
		return true;
	}

	bool Move(const FPointerEvent& Event)
	{
		if (!IsOpen())
		{
			return false;
		}
		const FVector2f Point = ToLocal(Event.GetScreenSpacePosition());
		LastPoint = Point;
		Handle(Gesture->Move(Point, NowMs()));
		if (IsOpen())
		{
			MovedTo(Point);
		}
		return true;
	}

	bool Release(const FPointerEvent& Event)
	{
		if (!IsOpen())
		{
			return false;
		}
		const FVector2f Point = ToLocal(Event.GetScreenSpacePosition());
		UE_LOG(LogTemp, Log, TEXT("RigExec marking menu: release %s at %s"), *Event.GetEffectingButton().ToString(), *Event.GetScreenSpacePosition().ToString());
		bPressed = false;
		const FResolvedItem* Over = ItemUnder(Point);
		Handle(Gesture->Release(Point, NowMs(), Over ? Over->Item.Id : FString()));
		return true;
	}

	bool Key(const FKeyEvent& Event)
	{
		if (IsOpen() && Event.GetKey() == EKeys::Escape)
		{
			UE_LOG(LogTemp, Log, TEXT("RigExec marking menu: escape"));
			Handle(Gesture->Cancel());
			return true;
		}
		return false;
	}

	void Tick()
	{
		if (!IsOpen())
		{
			return;
		}
		if (!ViewportWidget.IsValid() || !Context->Actor.IsValid())
		{
			UE_LOG(LogTemp, Log, TEXT("RigExec marking menu: lost viewport/actor"));
			Close();
			return;
		}
		Handle(Gesture->Tick(NowMs()));
		if (IsOpen())
		{
			MovedTo(LastPoint);
		}
	}

	// -- painting -----------------------------------------------------------------

	int32 Paint(const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId) const
	{
		if (!IsOpen() || !Gesture->IsShown())
		{
			return LayerId;
		}
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		if (!Widget)
		{
			return LayerId;
		}
		// Gesture space is the viewport widget's local space; the overlay
		// may sit at a different origin.
		const FGeometry& ViewGeometry = Widget->GetTickSpaceGeometry();
		auto Here = [&](const FVector2f& P) {
			return FVector2f(Geometry.AbsoluteToLocal(ViewGeometry.LocalToAbsolute(FVector2D(P))));
		};
		const FResolvedMenu& Menu = Gesture->GetMenu();
		const FVector2f Centre = Gesture->GetCentre();
		const TArray<FVector2f> Parents = Gesture->ParentCentres();

		// The way back: a faint trail through every menu above this one,
		// and a circle where each was centred.
		if (Parents.Num() > 0)
		{
			TArray<FVector2f> Trail = Parents;
			Trail.Add(Centre);
			for (int32 I = 0; I + 1 < Trail.Num(); ++I)
			{
				RigExecDraw::Dashed(Out, LayerId, Geometry, Here(Trail[I]), Here(Trail[I + 1]),
				                    FLinearColor(FColor(128, 128, 128, 140)), 2.0f);
			}
			for (int32 I = 0; I < Parents.Num(); ++I)
			{
				const bool bHot = I == LitBack;
				const FVector2f P = Here(Parents[I]);
				RigExecDraw::Disc(Out, LayerId + 1, Geometry, P, BackRadiusPx, bHot ? GLitFill : GFill);
				RigExecDraw::Ring(Out, LayerId + 2, Geometry, P, BackRadiusPx, bHot ? GLitBorder : GBorder, GBorderWidth);
				// An arrow pointing back, so the circle reads as "return".
				const float S = BackRadiusPx * 0.4f;
				RigExecDraw::Lines(Out, LayerId + 3, Geometry,
				                   {P + FVector2f(S * 0.4f, -S), P + FVector2f(-S * 0.6f, 0.0f), P + FVector2f(S * 0.4f, S)},
				                   bHot ? GLitText : GText, 2.0f);
			}
		}
		LayerId += 4;
		// The guide from the centre to the cursor, under everything.
		if (Cursor.IsSet())
		{
			RigExecDraw::Lines(Out, LayerId, Geometry, {Here(Centre), Here(Cursor.GetValue())}, GGuide, 3.0f);
		}
		// The centre mark: a grey ring inside a black one.
		const float Mark = FMath::Max(6.0f, Base * 0.5f) * 0.5f;
		RigExecDraw::Ring(Out, LayerId + 1, Geometry, Here(Centre), Mark, GGuide, 5.0f);
		RigExecDraw::Ring(Out, LayerId + 2, Geometry, Here(Centre), Mark + 2.0f, FLinearColor::Black, 2.0f);
		LayerId += 4;

		TMap<FString, FBox2f> Rects = ItemRects(Menu, Centre, Parents);
		for (const EDir Dir : FillOrder)
		{
			if (const FResolvedItem* Item = Menu.Slot(Dir))
			{
				const FBox2f Box = Rects[Item->Item.Id];
				const bool bLit = Item->Item.Id == Lit && Item->bEnabled;
				const FSlateBrush* Brush = !Item->bEnabled ? DisabledPillBrush.Get() : bLit ? LitPillBrush.Get() : PillBrush.Get();
				// MakeBox tints with what it is given, not the brush's own colour.
				FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(Box.GetSize(), FSlateLayoutTransform(Here(Box.Min))),
				                           Brush, ESlateDrawEffect::None, Brush->TintColor.GetSpecifiedColor());
				PaintLabel(Out, LayerId + 1, Geometry, Here, *Item, Box, bLit, 0);
			}
		}
		LayerId += 4;
		FBox2f Column;
		const TMap<FString, FBox2f> Rows = ColumnRects(Menu, Centre, Column);
		if (Menu.Overflow.Num() > 0)
		{
			FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(Column.GetSize(), FSlateLayoutTransform(Here(Column.Min))),
			                           PillBrush.Get(), ESlateDrawEffect::None, GFill);
			for (const FResolvedItem& Item : Menu.Overflow)
			{
				const FBox2f Row = Rows[Item.Item.Id];
				const bool bLit = Item.Item.Id == Lit && Item.bEnabled;
				if (bLit)
				{
					FSlateDrawElement::MakeBox(Out, LayerId + 1,
					                           Geometry.ToPaintGeometry(Row.GetSize(), FSlateLayoutTransform(Here(Row.Min))),
					                           FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, GLitFill);
				}
				PaintLabel(Out, LayerId + 2, Geometry, Here, Item, Row, bLit, -1);
			}
		}
		return LayerId + 4;
	}

private:
	const FSlateFontMeasure& Measure() const { return *FSlateApplication::Get().GetRenderer()->GetFontMeasureService(); }

	static FString Text(const FResolvedItem& Item)
	{
		return Item.IsSubmenu() ? Item.Label + TEXT("  ›") : Item.Label;
	}

	FVector2f Size(const FString& String) const
	{
		return FVector2f(float(Measure().Measure(String, Font).X) + Base * 2.2f, Base * 1.6f);
	}

	/** A label's box moved across its direction until no back circle lies
	 * under it: reaching for it must not go back. */
	static FBox2f Clear(FBox2f Box, EDir Dir, const TArray<FVector2f>& Avoid)
	{
		if (Avoid.Num() == 0)
		{
			return Box;
		}
		const float Reach = BackRadiusPx + 6.0f;
		const FVector2f D = DirectionVector(Dir);
		FVector2f Across;
		if (D.Y == 0.0f)
		{
			Across = FVector2f(0.0f, -1.0f);
		}
		else if (D.X == 0.0f)
		{
			Across = FVector2f(1.0f, 0.0f);
		}
		else
		{
			// The perpendicular pointing away from the horizontal axis.
			Across = D.Y * D.X > 0.0f ? FVector2f(D.Y, -D.X) : FVector2f(-D.Y, D.X);
			if (Across.Y * D.Y < 0.0f)
			{
				Across = -Across;
			}
			Across.Normalize();
		}
		for (int32 Step = 0; Step < 40; ++Step)
		{
			bool bHit = false;
			for (const FVector2f& A : Avoid)
			{
				if (Box.Intersect(FBox2f(A - FVector2f(Reach), A + FVector2f(Reach))))
				{
					bHit = true;
					break;
				}
			}
			if (!bHit)
			{
				break;
			}
			Box = Box.ShiftBy(Across * 4.0f);
		}
		return Box;
	}

	/** The ring's label boxes, anchored so each grows away from the centre. */
	TMap<FString, FBox2f> ItemRects(const FResolvedMenu& Menu, const FVector2f& Centre, const TArray<FVector2f>& Avoid) const
	{
		TMap<FString, FBox2f> Out;
		for (const EDir Dir : FillOrder)
		{
			const FResolvedItem* Item = Menu.Slot(Dir);
			if (!Item)
			{
				continue;
			}
			FVector2f WH = Size(Text(*Item));
			if (Item->Checked.Get(false))
			{
				WH.X += Base;
			}
			FVector2f At = Centre + Anchors[int32(Dir)];
			if (Dir == EDir::W || Dir == EDir::NW || Dir == EDir::SW)
			{
				At += FVector2f(-WH.X, -WH.Y * 0.5f);
			}
			else if (Dir == EDir::E || Dir == EDir::NE || Dir == EDir::SE)
			{
				At.Y -= WH.Y * 0.5f;
			}
			else if (Dir == EDir::N)
			{
				At += FVector2f(-WH.X * 0.5f, -WH.Y);
			}
			else
			{
				At.X -= WH.X * 0.5f;
			}
			Out.Add(Item->Item.Id, Inside(Clear(FBox2f(At, At + WH), Dir, Avoid)));
		}
		return Out;
	}

	/** The overflow column's rows, below the ring, and its box. */
	TMap<FString, FBox2f> ColumnRects(const FResolvedMenu& Menu, const FVector2f& Centre, FBox2f& OutBox) const
	{
		TMap<FString, FBox2f> Rows;
		if (Menu.Overflow.Num() == 0)
		{
			return Rows;
		}
		float Width = 0.0f;
		for (const FResolvedItem& Item : Menu.Overflow)
		{
			Width = FMath::Max(Width, Size(Text(Item)).X);
		}
		const float H = Base * 1.5f;
		const float X = Centre.X - Width * 0.5f;
		const float Y = Centre.Y + RadiusPx * 0.7f + Base * 3.0f;
		for (int32 I = 0; I < Menu.Overflow.Num(); ++I)
		{
			Rows.Add(Menu.Overflow[I].Item.Id, FBox2f(FVector2f(X, Y + I * H), FVector2f(X + Width, Y + (I + 1) * H)));
		}
		OutBox = FBox2f(FVector2f(X - GBorderWidth, Y - GBorderWidth),
		                FVector2f(X + Width + GBorderWidth, Y + H * Menu.Overflow.Num() + GBorderWidth));
		// The column moves as one onto the monitor.
		const FVector2f Shift = Inside(OutBox).Min - OutBox.Min;
		OutBox = OutBox.ShiftBy(Shift);
		for (TPair<FString, FBox2f>& Row : Rows)
		{
			Row.Value = Row.Value.ShiftBy(Shift);
		}
		return Rows;
	}

	/** The drawn item whose label is under a point: labels win over sectors,
	 * since a long label reaches into its neighbour's. */
	const FResolvedItem* ItemUnder(const FVector2f& Point) const
	{
		if (!IsOpen() || !Gesture->IsShown())
		{
			return nullptr;
		}
		const FResolvedMenu& Menu = Gesture->GetMenu();
		FBox2f Column;
		TMap<FString, FBox2f> Boxes = ItemRects(Menu, Gesture->GetCentre(), Gesture->ParentCentres());
		Boxes.Append(ColumnRects(Menu, Gesture->GetCentre(), Column));
		for (const TPair<FString, FBox2f>& Box : Boxes)
		{
			if (Box.Value.IsInsideOrOn(Point))
			{
				return Menu.Find(Box.Key);
			}
		}
		return nullptr;
	}

	template <typename FHere>
	void PaintLabel(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FHere& Here,
	                const FResolvedItem& Item, const FBox2f& Box, bool bLit, int32 Align) const
	{
		float Left = Box.Min.X + Base * 0.6f;
		const float Right = Box.Max.X - Base * 0.6f;
		const float Mid = (Box.Min.Y + Box.Max.Y) * 0.5f;
		if (Item.Checked.Get(false))
		{
			// A check mark for a toggle or radio that is on.
			const float Mark = Left + Base * 0.45f;
			const float S = Base * 0.3f;
			RigExecDraw::Lines(Out, Layer, Geometry,
			                   {Here(FVector2f(Mark - S, Mid)), Here(FVector2f(Mark - S * 0.3f, Mid + S * 0.7f)),
			                    Here(FVector2f(Mark + S, Mid - S * 0.8f))},
			                   bLit ? GLitText : GCheck, 2.0f);
			Left += Base;
		}
		const FString String = Text(Item);
		const FVector2f Extent = FVector2f(Measure().Measure(String, Font));
		const float X = Align < 0 ? Left : (Left + Right - Extent.X) * 0.5f;
		const FLinearColor Color = !Item.bEnabled ? GDisabledText : bLit ? GLitText : GText;
		FSlateDrawElement::MakeText(Out, Layer, Geometry.ToPaintGeometry(Extent, FSlateLayoutTransform(Here(FVector2f(X, Mid - Extent.Y * 0.5f)))),
		                            String, Font, ESlateDrawEffect::None, Color);
	}

	// -- the gesture ----------------------------------------------------------------

	FVector2f ToLocal(const FVector2D& Screen) const
	{
		const TSharedPtr<SViewport> Widget = ViewportWidget.Pin();
		return Widget ? FVector2f(Widget->GetTickSpaceGeometry().AbsoluteToLocal(Screen)) : FVector2f(Screen);
	}

	/** The controls a right-click at `Screen` acts on. A control under the
	 * cursor wins over the camera, whatever the modifiers, and is selected
	 * first if it is not already; with none, the selection takes a plain
	 * right-click anywhere in the viewport. Empty leaves the click to the
	 * viewport: nothing selected, or Alt (the camera's dolly) over empty
	 * space. */
	TArray<FName> Targets(const FVector2D& Screen, bool bAlt, ARigExecActor*& OutActor,
	                      TSharedPtr<SLevelViewport>& OutViewport) const
	{
		OutActor = nullptr;
		for (const TSharedPtr<SLevelViewport>& Viewport : LevelViewports())
		{
			const TSharedPtr<SViewport> Widget = Viewport ? Viewport->GetViewportWidget().Pin() : nullptr;
			if (Widget && Widget->IsHovered() && Widget->GetTickSpaceGeometry().IsUnderLocation(Screen))
			{
				OutViewport = Viewport;
				break;
			}
		}
		if (!OutViewport)
		{
			return {};
		}
		// A hover-picker button, then a Touch Pose region, then a control's
		// own shape: the topmost first.
		TArray<FName> Hovered = FRigExecHoverPicker::ControlsAt(Screen, OutActor);
		if (Hovered.Num() == 0)
		{
			const FName Touched = FRigExecTouchPose::ControlAt(Screen, OutActor);
			if (!Touched.IsNone())
			{
				Hovered.Add(Touched);
			}
		}
		if (Hovered.Num() == 0)
		{
			const FName Shape = ShapeAt(OutViewport, Screen, OutActor);
			if (!Shape.IsNone())
			{
				Hovered.Add(Shape);
			}
		}
		if (Hovered.Num() == 0)
		{
			if (bAlt)
			{
				return {};
			}
			UWorld* World = OutViewport->GetLevelViewportClient().GetWorld();
			for (TActorIterator<ARigExecActor> It(World); World && It; ++It)
			{
				const TArray<FName> Selected = SelectedControls(ControlRigOf(*It));
				if (Selected.Num() > 0)
				{
					OutActor = *It;
					return Selected;
				}
			}
			return {};
		}
		UControlRig* Rig = ControlRigOf(OutActor);
		if (!Rig)
		{
			return {};
		}
		const TArray<FName> Selected = SelectedControls(Rig);
		if (Hovered.ContainsByPredicate([&Selected](const FName& N) { return !Selected.Contains(N); }))
		{
			const FScopedTransaction Transaction(LOCTEXT("MarkingSelect", "Select Controls"));
			Rig->ClearControlSelection(true);
			for (const FName& Control : Hovered)
			{
				Rig->SelectControl(Control, true, true);
			}
			return Hovered;
		}
		return Selected;
	}

	/** A rig's selected controls, not their animation channels: a channel is
	 * part of its control, and the menu acts on controls. */
	static TArray<FName> SelectedControls(UControlRig* Rig)
	{
		if (!Rig)
		{
			return {};
		}
		TArray<FName> Selected = Rig->CurrentControlSelection();
		const URigHierarchy* Hierarchy = Rig->GetHierarchy();
		Selected.RemoveAll([Hierarchy](const FName& N) {
			const FRigControlElement* Element = Hierarchy->Find<FRigControlElement>(FRigElementKey(N, ERigElementType::Control));
			return !Element || Element->IsAnimationChannel();
		});
		return Selected;
	}

	/** The RigExec control whose shape is at or near `Screen`. */
	static FName ShapeAt(const TSharedPtr<SLevelViewport>& Viewport, const FVector2D& Screen, ARigExecActor*& OutActor)
	{
		FLevelEditorViewportClient& Client = Viewport->GetLevelViewportClient();
		FViewport* Target = Client.Viewport;
		const TSharedPtr<SViewport> Widget = Viewport->GetViewportWidget().Pin();
		if (!Target || !Widget || !Client.GetWorld())
		{
			return NAME_None;
		}
		const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
		const FVector2D Local = Geometry.AbsoluteToLocal(Screen);
		const FVector2D Size = Geometry.GetLocalSize();
		const FIntPoint Pixels = Target->GetSizeXY();
		if (Size.X <= 0.0 || Size.Y <= 0.0)
		{
			return NAME_None;
		}
		const FIntPoint Pixel(FMath::FloorToInt(Local.X * Pixels.X / Size.X), FMath::FloorToInt(Local.Y * Pixels.Y / Size.Y));
		const FIntRect Rect(FMath::Max(Pixel.X - GShapeSlop, 0), FMath::Max(Pixel.Y - GShapeSlop, 0),
		                    FMath::Min(Pixel.X + GShapeSlop + 1, Pixels.X), FMath::Min(Pixel.Y + GShapeSlop + 1, Pixels.Y));
		if (Rect.Area() <= 0)
		{
			return NAME_None;
		}
		TArray<HHitProxy*> Proxies;
		if (HHitProxy* Exact = Target->GetHitProxy(Pixel.X, Pixel.Y))
		{
			Proxies.Add(Exact);
		}
		Target->GetHitProxyMap(Rect, Proxies);
		for (HHitProxy* Proxy : Proxies)
		{
			const HActor* ActorProxy = HitProxyCast<HActor>(Proxy);
			const AControlRigShapeActor* Shape = ActorProxy ? Cast<AControlRigShapeActor>(ActorProxy->Actor) : nullptr;
			const UControlRig* ShapeRig = Shape ? Shape->ControlRig.Get() : nullptr;
			if (!ShapeRig)
			{
				continue;
			}
			// Only a RigExec character's controls.
			for (TActorIterator<ARigExecActor> It(Client.GetWorld()); It; ++It)
			{
				if (ControlRigOf(*It) == ShapeRig)
				{
					OutActor = *It;
					return Shape->ControlName;
				}
			}
		}
		return NAME_None;
	}

	void Open(const TSharedPtr<SLevelViewport>& Viewport, ARigExecActor* Actor, const TArray<FName>& Controls,
	          const FVector2D& Screen)
	{
		Close();
		ViewportWidget = Viewport->GetViewportWidget();
		Context = MakeUnique<FSelectionContext>(Actor, Controls);
		Gesture = MakeUnique<FGesture>(SelectionMenu(), *Context);
		// The editor window the viewport is in: the menu is drawn over all of
		// it, and labels stay inside it (and inside the monitor).
		HostWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.Pin().ToSharedRef());
		FSlateRect Area = FSlateApplication::Get().GetWorkArea(FSlateRect(Screen.X, Screen.Y, Screen.X + 1.0, Screen.Y + 1.0));
		if (const TSharedPtr<SWindow> Host = HostWindow.Pin())
		{
			const FSlateRect Client = Host->GetClientRectInScreen();
			Area = FSlateRect(FMath::Max(Area.Left, Client.Left), FMath::Max(Area.Top, Client.Top),
			                  FMath::Min(Area.Right, Client.Right), FMath::Min(Area.Bottom, Client.Bottom));
		}
		Bounds = FBox2f(ToLocal(FVector2D(Area.Left, Area.Top)), ToLocal(FVector2D(Area.Right, Area.Bottom)));
		LastPoint = ToLocal(Screen);
		Handle(Gesture->Press(LastPoint, NowMs()));
	}

	void Close()
	{
		Gesture.Reset();
		Context.Reset();
		bPressed = false;
		Cursor.Reset();
		Lit.Reset();
		SectorLit.Reset();
		LitBack = INDEX_NONE;
		if (Overlay)
		{
			if (const TSharedPtr<SWindow> Host = HostWindow.Pin())
			{
				Host->RemoveOverlaySlot(Overlay.ToSharedRef());
			}
			Overlay.Reset();
		}
	}

	/** The menu's paint layer: an overlay on the editor window the viewport
	 * is in, above every panel in it, so the menu is never clipped to the
	 * viewport. (A separate transparent window would need the OS to
	 * composite it, which the RHI renderer does not; it draws black.) Made
	 * when the menu is first drawn: a flick never needs one. */
	void ShowWindow()
	{
		const TSharedPtr<SWindow> Host = HostWindow.Pin();
		if (Overlay || !Host || !Host->HasOverlay())
		{
			return;
		}
		Overlay = SNew(SRigExecMarkingOverlay);
		Host->AddOverlaySlot(TNumericLimits<int32>::Max())
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				Overlay.ToSharedRef()
			];
	}

	/** A box moved, not resized, until it lies inside the editor window. */
	FBox2f Inside(const FBox2f& Box) const
	{
		constexpr float Margin = 4.0f;
		FVector2f Shift = FVector2f::ZeroVector;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (Box.Min[Axis] < Bounds.Min[Axis] + Margin)
			{
				Shift[Axis] = Bounds.Min[Axis] + Margin - Box.Min[Axis];
			}
			else if (Box.Max[Axis] > Bounds.Max[Axis] - Margin)
			{
				Shift[Axis] = Bounds.Max[Axis] - Margin - Box.Max[Axis];
			}
		}
		return Box.ShiftBy(Shift);
	}

	void Handle(const TArray<FEmission>& Emissions)
	{
		for (const FEmission& Emission : Emissions)
		{
			switch (Emission.Kind)
			{
			case EEmit::Show:
			case EEmit::Open:
				ShowWindow();
				Lit.Reset();
				SectorLit.Reset();
				LitBack = INDEX_NONE;
				break;
			case EEmit::Highlight:
				SectorLit = Emission.Item.IsSet() ? Emission.Item->Item.Id : FString();
				Lit = SectorLit;
				break;
			case EEmit::Confirm:
			{
				// The context outlives the gesture only as long as Run needs.
				const FResolvedItem Item = Emission.Item.GetValue();
				TUniquePtr<FSelectionContext> Ran = MoveTemp(Context);
				Close();
				Run(Item, *Ran);
				return;
			}
			case EEmit::Cancel:
				UE_LOG(LogTemp, Log, TEXT("RigExec marking menu: cancel"));
				Close();
				return;
			default:
				break;
			}
		}
	}

	void MovedTo(const FVector2f& Point)
	{
		if (!Gesture->IsShown())
		{
			return;
		}
		const bool bStroke = Gesture->GetMode() == EMode::Flick && bPressed;
		Cursor = bStroke ? TOptional<FVector2f>(Point) : TOptional<FVector2f>();
		// A label or column row under the cursor is what lights, held or
		// clicked; elsewhere a held stroke lights its direction.
		if (const FResolvedItem* Over = ItemUnder(Point))
		{
			Lit = Over->Item.Id;
		}
		else
		{
			Lit = bStroke ? SectorLit : FString();
		}
		LitBack = Gesture->BackTarget(Point);
	}

	/** A press while the menu is up for clicking: a label or column row
	 * under it is chosen outright; anything else is the model's. */
	void ClickAt(const FVector2f& Point)
	{
		if (Gesture->BackTarget(Point) != INDEX_NONE)
		{
			Handle(Gesture->Press(Point, NowMs()));
			return;
		}
		if (const FResolvedItem* Over = ItemUnder(Point))
		{
			if (Over->bEnabled)
			{
				const FString Id = Over->Item.Id;
				Handle(Gesture->Choose(Id, NowMs()));
			}
			return;
		}
		Handle(Gesture->Press(Point, NowMs()));
	}

	// -- the commands -------------------------------------------------------------

	void Run(const FResolvedItem& Resolved, const FSelectionContext& Ran)
	{
		const FString& Name = Resolved.Item.Write;
		ARigExecActor* Actor = Ran.Actor.Get();
		UControlRig* Rig = ControlRigOf(Actor);
		URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
		if (!Hierarchy)
		{
			return;
		}
		auto Channels = [Hierarchy](const FName& Control, bool bWithChannels) {
			TArray<FRigElementKey> Keys = {FRigElementKey(Control, ERigElementType::Control)};
			if (bWithChannels)
			{
				for (const FRigElementKey& Child : Hierarchy->GetChildren(Keys[0]))
				{
					const FRigControlElement* Element = Hierarchy->Find<FRigControlElement>(Child);
					if (Element && Element->IsAnimationChannel())
					{
						Keys.Add(Child);
					}
				}
			}
			return Keys;
		};
		auto ToInitial = [&](bool bWithChannels, bool bTransformOnly, const FText& Title) {
			const FScopedTransaction Transaction(Title);
			const FRigControlModifiedContext Change(EControlRigSetKey::DoNotCare);
			int32 Count = 0;
			for (const FName& Control : Ran.Controls)
			{
				for (const FRigElementKey& Key : Channels(Control, bWithChannels))
				{
					FRigControlElement* Element = Hierarchy->Find<FRigControlElement>(Key);
					if (!Element || (bTransformOnly && Element->IsAnimationChannel()))
					{
						continue;
					}
					Rig->SetControlValueImpl(Key.Name, Hierarchy->GetControlValue(Element, ERigControlValueType::Initial), true,
					                         Change, true);
					++Count;
				}
			}
			return Count;
		};

		if (Name == TEXT("KeySelection"))
		{
			// Sequencer keys a control whose change says "always key"; with no
			// sequence the rig has no track to key into.
			if (!Rig->GetTypedOuter<UMovieSceneControlRigParameterTrack>())
			{
				Notify(LOCTEXT("KeyNoSequence", "Key: open the level sequence with the character's Control Rig track first."));
				return;
			}
			const FScopedTransaction Transaction(LOCTEXT("MarkingKey", "Key Controls"));
			const FRigControlModifiedContext Change(EControlRigSetKey::Always);
			// Keying a channel selects it; the selection is put back after.
			const TArray<FName> Before = Rig->CurrentControlSelection();
			int32 Count = 0;
			for (const FName& Control : Ran.Controls)
			{
				for (const FRigElementKey& Key : Channels(Control, true))
				{
					if (FRigControlElement* Element = Hierarchy->Find<FRigControlElement>(Key))
					{
						Rig->SetControlValueImpl(Key.Name, Hierarchy->GetControlValue(Element, ERigControlValueType::Current), true,
						                         Change, true);
						++Count;
					}
				}
			}
			if (Rig->CurrentControlSelection() != Before)
			{
				Rig->ClearControlSelection(true);
				for (const FName& Control : Before)
				{
					Rig->SelectControl(Control, true, true);
				}
			}
			Notify(FText::Format(LOCTEXT("Keyed", "Keyed {0} channels on {1} controls"), Count, Ran.Controls.Num()));
		}
		else if (Name == TEXT("ResetToRest"))
		{
			// The transforms only; channels are Zero pose's.
			ToInitial(false, true, LOCTEXT("MarkingReset", "Reset to Rest"));
		}
		else if (Name == TEXT("ZeroPose"))
		{
			// Every pose channel, as the picker's Zero Ctrls: an IK/FK dial, a
			// space and a foot roll come back as well as the transforms.
			ToInitial(true, false, LOCTEXT("MarkingZero", "Zero Pose"));
		}
		else if (Name == TEXT("FrameSelection"))
		{
			const FTransform ToWorld = Actor->Controls->GetComponentTransform();
			FBox Box(ForceInit);
			for (const FName& Control : Ran.Controls)
			{
				Box += ToWorld.TransformPosition(
					Hierarchy->GetGlobalTransform(FRigElementKey(Control, ERigElementType::Control)).GetLocation());
			}
			if (Box.IsValid && GEditor)
			{
				GEditor->MoveViewportCamerasToBox(Box.ExpandBy(FMath::Max(Box.GetExtent().GetMax(), 15.0)), true);
			}
		}
		else if (Name == TEXT("ToggleFkIk"))
		{
			const FString Switch = Ran.LimbSwitch();
			if (!Switch.IsEmpty())
			{
				const FScopedTransaction Transaction(LOCTEXT("MarkingSwitch", "Switch Limb"));
				if (!Ran.Rig()->SwitchLimb(Switch))
				{
					Notify(LOCTEXT("SwitchFailed", "IK/FK match failed."));
				}
			}
		}
		else if (Name == TEXT("SetSpace"))
		{
			const FScopedTransaction Transaction(LOCTEXT("MarkingSpace", "Switch Space"));
			Ran.Rig()->SwitchSpace(Ran.Controls.Last(), int32(Resolved.Item.Value));
		}
		else if (Name == TEXT("SelectCounterpart"))
		{
			TArray<FName> Found;
			for (const FName& Control : Ran.Controls)
			{
				const FString Other = CounterpartName(Control.ToString());
				if (!Other.IsEmpty() && Hierarchy->Contains(FRigElementKey(FName(Other), ERigElementType::Control)))
				{
					Found.AddUnique(FName(Other));
				}
			}
			if (Found.Num() == 0)
			{
				Notify(LOCTEXT("NoCounterpart", "No mirrored control for the selection."));
				return;
			}
			const FScopedTransaction Transaction(LOCTEXT("MarkingCounterpart", "Select Counterpart"));
			Rig->ClearControlSelection(true);
			for (const FName& Control : Found)
			{
				Rig->SelectControl(Control, true, true);
			}
		}
		else if (Name == TEXT("ShowViewportMenu"))
		{
			FLevelEditorModule* Module = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
			if (const TSharedPtr<ILevelEditor> Editor = Module ? Module->GetFirstLevelEditor() : nullptr)
			{
				Editor->SummonLevelViewportContextMenu();
			}
		}
		else
		{
			Notify(FText::Format(LOCTEXT("NotYet", "{0}: not available yet"), FText::FromString(Resolved.Label)));
		}
	}

	FSlateFontInfo Font;
	float Base = 12.0f;
	FVector2f Anchors[8];
	TUniquePtr<FSlateRoundedBoxBrush> PillBrush, LitPillBrush, DisabledPillBrush;

	TWeakPtr<SViewport> ViewportWidget;
	TWeakPtr<SWindow> HostWindow;
	TSharedPtr<SRigExecMarkingOverlay> Overlay;
	/** The editor window's client area within the monitor, gesture space. */
	FBox2f Bounds = FBox2f(FVector2f(-1e6f), FVector2f(1e6f));
	TUniquePtr<FSelectionContext> Context;
	TUniquePtr<FGesture> Gesture;
	bool bPressed = false;
	FVector2f LastPoint = FVector2f::ZeroVector;
	TOptional<FVector2f> Cursor;
	FString Lit;
	FString SectorLit;
	int32 LitBack = INDEX_NONE;
};

// -- the overlay ------------------------------------------------------------------

void
SRigExecMarkingOverlay::Construct(const FArguments&)
{
	SetVisibility(EVisibility::HitTestInvisible);
	ForceVolatile(true);
}

int32
SRigExecMarkingOverlay::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
                                FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle&, bool) const
{
	return GMarkingMenu ? GMarkingMenu->Paint(Geometry, Out, LayerId) : LayerId;
}

// -- the processor ----------------------------------------------------------------

FRigExecMarkingMenu::FRigExecMarkingMenu() : Controller(MakeUnique<FRigExecMarkingMenuController>()) {}

FRigExecMarkingMenu::~FRigExecMarkingMenu() = default;

void
FRigExecMarkingMenu::Startup()
{
	if (GMarkingMenu || !FSlateApplication::IsInitialized())
	{
		return;
	}
	GMarkingMenu = MakeShared<FRigExecMarkingMenu>();
	// First in line: an open menu is modal, and a right-click on a picker
	// button or a touch region is the menu's before either sees it.
	FSlateApplication::Get().RegisterInputPreProcessor(GMarkingMenu, 0);
}

void
FRigExecMarkingMenu::Shutdown()
{
	if (!GMarkingMenu)
	{
		return;
	}
	GMarkingMenu->Controller.Reset();
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(GMarkingMenu);
	}
	GMarkingMenu.Reset();
}

bool
FRigExecMarkingMenu::IsOpen()
{
	return GMarkingMenu && GMarkingMenu->Controller && GMarkingMenu->Controller->IsOpen();
}

void
FRigExecMarkingMenu::Tick(const float, FSlateApplication&, TSharedRef<ICursor>)
{
	if (Controller)
	{
		Controller->Tick();
	}
}

bool
FRigExecMarkingMenu::HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& InKeyEvent)
{
	return Controller && Controller->Key(InKeyEvent);
}

bool
FRigExecMarkingMenu::HandleMouseMoveEvent(FSlateApplication&, const FPointerEvent& MouseEvent)
{
	return Controller && Controller->Move(MouseEvent);
}

bool
FRigExecMarkingMenu::HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent& MouseEvent)
{
	if (Controller && Controller->Press(MouseEvent))
	{
		SwallowUp = MouseEvent.GetEffectingButton();
		return true;
	}
	return false;
}

bool
FRigExecMarkingMenu::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	return HandleMouseButtonDownEvent(SlateApp, MouseEvent);
}

bool
FRigExecMarkingMenu::HandleMouseButtonUpEvent(FSlateApplication&, const FPointerEvent& MouseEvent)
{
	const FKey Button = MouseEvent.GetEffectingButton();
	const bool bOurs = SwallowUp.IsValid() && Button == SwallowUp;
	if (bOurs)
	{
		SwallowUp = FKey();
	}
	if (Controller && Controller->Release(MouseEvent))
	{
		return true;
	}
	// The release of a press the menu took, after the menu closed on it.
	return bOurs;
}

int32
FRigExecMarkingMenu::Paint(const FGeometry& Geometry, FSlateWindowElementList& Out, int32 LayerId) const
{
	return Controller ? Controller->Paint(Geometry, Out, LayerId) : LayerId;
}

#undef LOCTEXT_NAMESPACE
