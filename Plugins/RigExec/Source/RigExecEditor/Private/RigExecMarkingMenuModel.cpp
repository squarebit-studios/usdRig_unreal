#include "RigExecMarkingMenuModel.h"

namespace RigExecMarking
{
const EDir FillOrder[8] = {EDir::W, EDir::E, EDir::S, EDir::N, EDir::NW, EDir::NE, EDir::SW, EDir::SE};

FVector2f
DirectionVector(EDir Dir)
{
	constexpr float D = 0.70710678f;
	switch (Dir)
	{
	case EDir::E: return FVector2f(1.0f, 0.0f);
	case EDir::NE: return FVector2f(D, -D);
	case EDir::N: return FVector2f(0.0f, -1.0f);
	case EDir::NW: return FVector2f(-D, -D);
	case EDir::W: return FVector2f(-1.0f, 0.0f);
	case EDir::SW: return FVector2f(-D, D);
	case EDir::S: return FVector2f(0.0f, 1.0f);
	case EDir::SE: return FVector2f(D, D);
	default: return FVector2f::ZeroVector;
	}
}

const TCHAR*
DirName(EDir Dir)
{
	static const TCHAR* Names[] = {TEXT("E"), TEXT("NE"), TEXT("N"), TEXT("NW"), TEXT("W"), TEXT("SW"), TEXT("S"), TEXT("SE"), TEXT("-")};
	return Names[int32(Dir)];
}

EDir
SectorFor(const FVector2f& Offset, float Threshold)
{
	if (Offset.X * Offset.X + Offset.Y * Offset.Y < Threshold * Threshold)
	{
		return EDir::None;
	}
	// atan2 wants a mathematical y, so the screen one is negated exactly
	// here; half a sector added before the divide centres the sectors on
	// the directions, and the modulos make the +/-180 wrap a non-event.
	const double Angle = FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(double(-Offset.Y), double(Offset.X))) + 360.0, 360.0);
	const int32 Index = int32(FMath::Fmod(Angle + 22.5, 360.0) / 45.0) % 8;
	return EDir(Index);
}

FVector2f
PointFor(const FVector2f& Centre, EDir Dir, float Radius)
{
	return Centre + DirectionVector(Dir) * Radius;
}

// -- Resolution and layout ---------------------------------------------------

const FResolvedItem*
FResolvedMenu::Slot(EDir Dir) const
{
	return Dir != EDir::None && Slots[int32(Dir)].IsSet() ? &Slots[int32(Dir)].GetValue() : nullptr;
}

const FResolvedItem*
FResolvedMenu::Find(const FString& Id) const
{
	for (const EDir Dir : FillOrder)
	{
		if (const FResolvedItem* Item = Slot(Dir); Item && Item->Item.Id == Id)
		{
			return Item;
		}
	}
	return Overflow.FindByPredicate([&Id](const FResolvedItem& R) { return R.Item.Id == Id; });
}

void
Layout(TArray<FResolvedItem>& Items, FResolvedMenu& Out)
{
	TArray<FResolvedItem*> Fill;
	for (FResolvedItem& Item : Items)
	{
		if (Item.Item.bOverflow)
		{
			Item.Direction = EDir::None;
			Out.Overflow.Add(Item);
			continue;
		}
		const EDir Pin = Item.Item.Dir;
		if (Pin != EDir::None && !Out.Slots[int32(Pin)].IsSet())
		{
			Item.Direction = Pin;
			Out.Slots[int32(Pin)] = Item;
		}
		else
		{
			Fill.Add(&Item);
		}
	}
	TArray<EDir> Free;
	for (const EDir Dir : FillOrder)
	{
		if (!Out.Slots[int32(Dir)].IsSet())
		{
			Free.Add(Dir);
		}
	}
	for (FResolvedItem* Item : Fill)
	{
		if (Free.Num() > 0)
		{
			Item->Direction = Free[0];
			Free.RemoveAt(0);
			Out.Slots[int32(Item->Direction)] = *Item;
		}
		else
		{
			Item->Direction = EDir::None;
			Out.Overflow.Add(*Item);
		}
	}
}

namespace
{
TOptional<bool>
CheckedState(const FItem& Item, const IContext& Context)
{
	if (Item.Kind == EKind::Toggle)
	{
		const TOptional<double> Value = Context.Read(Item.Read);
		return Value.IsSet() && Value.GetValue() != 0.0;
	}
	if (Item.Kind == EKind::Radio)
	{
		const TOptional<double> Value = Context.Read(Item.Read);
		return Value.IsSet() && FMath::IsNearlyEqual(Value.GetValue(), Item.Value);
	}
	return {};
}
} // namespace

FResolvedMenu
Resolve(const FMenu& Menu, const IContext& Context, EMode Mode)
{
	FResolvedMenu Out;
	Out.Source = Menu;
	Out.Mode = Mode;
	TArray<FResolvedItem> Items;
	for (const FItem& Item : Menu.Items)
	{
		const bool bAvailable = Item.When.IsEmpty() || Context.Test(Item.When);
		if (!bAvailable && Mode == EMode::Flick)
		{
			continue;
		}
		FResolvedItem& Resolved = Items.AddDefaulted_GetRef();
		Resolved.Item = Item;
		Resolved.Label = Context.Label(Item.Id, Item.Label);
		Resolved.bEnabled = bAvailable;
		Resolved.Checked = CheckedState(Item, Context);
	}
	Layout(Items, Out);
	return Out;
}

FResolvedMenu
ResolveChild(const FItem& Item, const IContext& Context, EMode Mode)
{
	FMenu Child;
	Child.Id = Item.Id;
	Child.Label = Item.Label;
	Child.Items = Item.Items;
	if (!Item.Children.IsEmpty())
	{
		Child.Items.Append(Context.Children(Item.Children));
	}
	return Resolve(Child, Context, Mode);
}

// -- The gesture -------------------------------------------------------------

namespace
{
FEmission
Emit(EEmit Kind)
{
	FEmission E;
	E.Kind = Kind;
	return E;
}
} // namespace

FGesture::FGesture(const FMenu& InMenu, const IContext& InContext) : Menu(InMenu), Context(InContext)
{
	Reset();
}

void
FGesture::Reset()
{
	State = EState::Idle;
	Mode = EMode::Flick;
	Resolved = FResolvedMenu();
	Stack.Reset();
	Direction = EDir::None;
	bShown = false;
	Highlighted = EDir::None;
	bHighlightReported = false;
}

TArray<FVector2f>
FGesture::ParentCentres() const
{
	TArray<FVector2f> Out;
	for (const TPair<FResolvedMenu, FVector2f>& Entry : Stack)
	{
		Out.Add(Entry.Value);
	}
	return Out;
}

int32
FGesture::BackTarget(const FVector2f& At) const
{
	for (int32 Index = Stack.Num() - 1; Index >= 0; --Index)
	{
		if (FVector2f::Distance(Stack[Index].Value, At) <= BackRadiusPx)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

TArray<FEmission>
FGesture::Press(const FVector2f& At, double Ms)
{
	if (State == EState::Click)
	{
		return ClickPress(At, Ms);
	}
	if (State == EState::Armed || State == EState::Stroke)
	{
		// A second button mid-stroke: keep the stroke.
		return {Emit(EEmit::Idle)};
	}
	Reset();
	State = EState::Armed;
	Mode = EMode::Flick;
	Centre = Point = At;
	PressTime = DwellFrom = Ms;
	// Resolved now, not at the show: a flick can be over in 80 ms and still
	// has to know what W meant.
	Resolved = Resolve(Menu, Context, EMode::Flick);
	return {Emit(EEmit::Idle)};
}

TArray<FEmission>
FGesture::Move(const FVector2f& At, double Ms)
{
	if (State == EState::Idle)
	{
		return {Emit(EEmit::Idle)};
	}
	Point = At;
	const int32 BackTo = BackTarget(At);
	if (BackTo != INDEX_NONE)
	{
		return Back(BackTo, Ms);
	}
	TArray<FEmission> Out;
	if (State == EState::Armed || State == EState::Stroke)
	{
		Out.Append(MaybeShow(Ms));
	}
	Out.Append(Aim(Ms));
	Out.Append(MaybeUnfold(Ms));
	return Out.Num() ? Out : TArray<FEmission>{Emit(EEmit::Idle)};
}

TArray<FEmission>
FGesture::Tick(double Ms)
{
	if (State == EState::Idle)
	{
		return {Emit(EEmit::Idle)};
	}
	TArray<FEmission> Out;
	if (State == EState::Armed || State == EState::Stroke)
	{
		Out.Append(MaybeShow(Ms));
		Out.Append(Aim(Ms));
	}
	Out.Append(MaybeUnfold(Ms));
	return Out.Num() ? Out : TArray<FEmission>{Emit(EEmit::Idle)};
}

TArray<FEmission>
FGesture::Release(const FVector2f& At, double Ms, const FString& ItemId)
{
	if (State == EState::Idle || State == EState::Click)
	{
		return {Emit(EEmit::Idle)};
	}
	Point = At;
	const EDir Dir = SectorFor(Point - Centre);
	if (!ItemId.IsEmpty() && bShown)
	{
		if (const FResolvedItem* Named = Resolved.Find(ItemId))
		{
			const FResolvedItem Copy = *Named;
			return ReleaseOn(Copy, Ms);
		}
	}
	if (Dir == EDir::None)
	{
		// Quick enough and it was a tap: up for clicking. Slow, and it was
		// a hold that went nowhere, which is how a marking menu is backed
		// out of.
		return Since(Ms) < TapMs ? EnterClickMode(Ms) : CancelWith();
	}
	const FResolvedItem* Item = Resolved.Slot(Dir);
	if (!Item)
	{
		return CancelWith();
	}
	const FResolvedItem Copy = *Item;
	return ReleaseOn(Copy, Ms);
}

TArray<FEmission>
FGesture::ReleaseOn(const FResolvedItem& Item, double Now)
{
	if (!Item.bEnabled)
	{
		return CancelWith();
	}
	if (Item.IsSubmenu())
	{
		// A submenu the stroke never pulled far enough to unfold: open it
		// under the cursor, up for clicking.
		Mode = EMode::Click;
		State = EState::Click;
		return Unfold(Item, Point, Now);
	}
	return Confirm(Item);
}

TArray<FEmission>
FGesture::Choose(const FString& ItemId, double Ms)
{
	if (State == EState::Idle)
	{
		return {Emit(EEmit::Idle)};
	}
	const FResolvedItem* Item = Resolved.Find(ItemId);
	if (!Item || !Item->bEnabled)
	{
		return {Emit(EEmit::Idle)};
	}
	const FResolvedItem Copy = *Item;
	return Copy.IsSubmenu() ? Unfold(Copy, Point, Ms) : Confirm(Copy);
}

TArray<FEmission>
FGesture::Cancel()
{
	return State == EState::Idle ? TArray<FEmission>{Emit(EEmit::Idle)} : CancelWith();
}

TArray<FEmission>
FGesture::MaybeShow(double Now)
{
	if (bShown || Since(Now) < ShowMs)
	{
		return {};
	}
	bShown = true;
	State = EState::Stroke;
	return {Emit(EEmit::Show)};
}

TArray<FEmission>
FGesture::Aim(double Now)
{
	// Tracked before the show too, so the menu appears with the right slot
	// already lit.
	const EDir Dir = SectorFor(Point - Centre);
	if (Dir != Direction)
	{
		Direction = Dir;
		DwellFrom = Now;
	}
	if (!bShown || (bHighlightReported && Dir == Highlighted))
	{
		return {};
	}
	Highlighted = Dir;
	bHighlightReported = true;
	FEmission E = Emit(EEmit::Highlight);
	if (const FResolvedItem* Item = Resolved.Slot(Dir))
	{
		E.Item = *Item;
	}
	return {E};
}

TArray<FEmission>
FGesture::MaybeUnfold(double Now)
{
	if (!bShown || Direction == EDir::None)
	{
		return {};
	}
	const FResolvedItem* Item = Resolved.Slot(Direction);
	if (!Item || !Item->IsSubmenu() || !Item->bEnabled)
	{
		return {};
	}
	const FResolvedItem Copy = *Item;
	const float Reach = FVector2f::Distance(Centre, Point);
	// The stroke that keeps going recentres under the cursor; the dwell
	// recentres on the item's own label.
	if (Reach >= RadiusPx)
	{
		return Unfold(Copy, Point, Now);
	}
	// Just back from this submenu, resting on its circle: do not reopen.
	if (Reach < BackRadiusPx)
	{
		return {};
	}
	if (Now - DwellFrom >= UnfoldMs)
	{
		return Unfold(Copy, PointFor(Centre, Direction), Now);
	}
	return {};
}

TArray<FEmission>
FGesture::Unfold(const FResolvedItem& Item, const FVector2f& At, double Now)
{
	FResolvedMenu Child = ResolveChild(Item.Item, Context, Mode);
	Stack.Emplace(MoveTemp(Resolved), Centre);
	Resolved = MoveTemp(Child);
	Centre = At;
	Direction = EDir::None;
	Highlighted = EDir::None;
	bHighlightReported = false;
	DwellFrom = Now;
	bShown = true;
	return {Emit(EEmit::Open)};
}

TArray<FEmission>
FGesture::Back(int32 Index, double Now)
{
	Resolved = Stack[Index].Key;
	Centre = Stack[Index].Value;
	Stack.SetNum(Index);
	Direction = EDir::None;
	Highlighted = EDir::None;
	bHighlightReported = false;
	DwellFrom = Now;
	return {Emit(EEmit::Open)};
}

TArray<FEmission>
FGesture::EnterClickMode(double Now)
{
	// Resolved again: what was hidden mid-flick becomes a greyed-out label
	// the artist can see.
	Mode = EMode::Click;
	State = EState::Click;
	Resolved = Resolve(Resolved.Source, Context, EMode::Click);
	Direction = EDir::None;
	Highlighted = EDir::None;
	bHighlightReported = false;
	DwellFrom = Now;
	bShown = true;
	return {Emit(EEmit::Show)};
}

TArray<FEmission>
FGesture::ClickPress(const FVector2f& At, double Now)
{
	Point = At;
	const int32 BackTo = BackTarget(At);
	if (BackTo != INDEX_NONE)
	{
		return Back(BackTo, Now);
	}
	const EDir Dir = SectorFor(Point - Centre);
	if (Dir == EDir::None || FVector2f::Distance(Centre, Point) > OutsidePx)
	{
		return CancelWith();
	}
	const FResolvedItem* Item = Resolved.Slot(Dir);
	if (!Item || !Item->bEnabled)
	{
		return {Emit(EEmit::Idle)};
	}
	const FResolvedItem Copy = *Item;
	return Copy.IsSubmenu() ? Unfold(Copy, Point, Now) : Confirm(Copy);
}

TArray<FEmission>
FGesture::Confirm(const FResolvedItem& Item)
{
	FEmission E = Emit(EEmit::Confirm);
	E.Item = Item;
	E.bShown = bShown;
	Reset();
	return {E};
}

TArray<FEmission>
FGesture::CancelWith()
{
	Reset();
	return {Emit(EEmit::Cancel)};
}

// -- The built-in menus ------------------------------------------------------

namespace
{
FItem
MakeItem(const TCHAR* Id, const TCHAR* Label, EKind Kind, EDir Dir, const TCHAR* Write, const TCHAR* When = TEXT(""),
         const TCHAR* Read = TEXT(""), const TCHAR* Children = TEXT(""), bool bOverflow = false)
{
	FItem Item;
	Item.Id = Id;
	Item.Label = Label;
	Item.Kind = Kind;
	Item.Dir = Dir;
	Item.Write = Write;
	Item.When = When;
	Item.Read = Read;
	Item.Children = Children;
	Item.bOverflow = bOverflow;
	return Item;
}
} // namespace

const FMenu&
SelectionMenu()
{
	static const FMenu Menu = []() {
		FMenu M;
		M.Id = TEXT("selection");
		M.Label = TEXT("Selection");
		// W and E: the two most-used commands take the two easiest flicks.
		M.Items.Add(MakeItem(TEXT("selection.key"), TEXT("Key"), EKind::Action, EDir::W, TEXT("KeySelection")));
		M.Items.Add(MakeItem(TEXT("selection.reset"), TEXT("Reset to rest"), EKind::Action, EDir::E, TEXT("ResetToRest")));
		// The vertical pair: "put things back where I can see them".
		M.Items.Add(MakeItem(TEXT("selection.zero"), TEXT("Zero pose"), EKind::Action, EDir::S, TEXT("ZeroPose")));
		M.Items.Add(MakeItem(TEXT("selection.frame"), TEXT("Frame"), EKind::Action, EDir::N, TEXT("FrameSelection")));
		// The conditional items take diagonals: hidden mid-flick, greyed
		// out when the menu is up to be read.
		M.Items.Add(MakeItem(TEXT("selection.fkik"), TEXT("FK/IK toggle"), EKind::Toggle, EDir::NW, TEXT("ToggleFkIk"),
		                     TEXT("limb"), TEXT("limbIsIk")));
		M.Items.Add(MakeItem(TEXT("selection.space"), TEXT("Space"), EKind::Submenu, EDir::NE, TEXT(""), TEXT("hasSpaces"),
		                     TEXT(""), TEXT("spaces")));
		M.Items.Add(MakeItem(TEXT("selection.counterpart"), TEXT("Select counterpart"), EKind::Action, EDir::SW,
		                     TEXT("SelectCounterpart"), TEXT("control")));
		// The column below the ring: the viewport's own menu, long and
		// rarely wanted, never promoted into the ring.
		M.Items.Add(MakeItem(TEXT("selection.viewportMenu"), TEXT("Viewport menu…"), EKind::Action, EDir::None,
		                     TEXT("ShowViewportMenu"), TEXT(""), TEXT(""), TEXT(""), true));
		return M;
	}();
	return Menu;
}
} // namespace RigExecMarking
