// The marking menu: what the gesture MEANS, with no Slate. A port of
// usdRig's markingMenuModel.py (and the Selection menu of
// markingMenuDefs.py).
//
// Press and the menu does not appear at once; flick toward a compass
// direction and release and the command runs with nothing ever drawn;
// hesitate and the ring fades in around the press point so the same eight
// directions can be read rather than remembered.
//
// This owns four things and nothing else:
//
//   * the RADIAL LAYOUT -- which of the eight slots each item lands in, and
//     what spills into the overflow column;
//   * the SECTOR RESOLUTION -- which direction a cursor offset means;
//   * the GESTURE STATE MACHINE -- press/move/release/tick in, emissions
//     out, with time passed in so the thresholds are exact;
//   * RESOLUTION -- turning the declared menu into a menu for THIS
//     selection, with labels, enablement and check marks filled in.
//
// THE Y AXIS POINTS DOWN: every point here is in Slate's local space, so
// North -- the top of the screen -- is NEGATIVE dy.
#pragma once

#include "CoreMinimal.h"

namespace RigExecMarking
{
// -- Timings and distances (Slate units, milliseconds) -----------------------
constexpr float ThresholdPx = 12.0f;   // dead zone around the centre
constexpr float RadiusPx = 96.0f;      // the ring, and where a submenu unfolds
constexpr double TapMs = 200.0;        // quicker, with no direction: a tap
constexpr double ShowMs = 150.0;       // hold this long and the menu is drawn
constexpr double UnfoldMs = 250.0;     // dwell this long and a submenu opens
constexpr float OutsidePx = RadiusPx * 2.0f;  // a click past this cancels
constexpr float BackRadiusPx = 16.0f;  // a parent menu's circle

// -- The eight directions ----------------------------------------------------
enum class EDir : uint8 { E, NE, N, NW, W, SW, S, SE, None };
/** The order unpinned items take slots in: the flicks a wrist makes most
 * accurately first. */
extern const EDir FillOrder[8];
/** Unit vectors, y down. */
FVector2f DirectionVector(EDir Dir);
const TCHAR* DirName(EDir Dir);
/** Which direction an offset from the centre means; None in the dead zone.
 * Sectors are 45 degrees, centred on the directions; a point exactly on a
 * boundary goes to the counter-clockwise sector. */
EDir SectorFor(const FVector2f& Offset, float Threshold = ThresholdPx);
/** Where a direction's label sits. */
FVector2f PointFor(const FVector2f& Centre, EDir Dir, float Radius = RadiusPx);

// -- Item and menu records ---------------------------------------------------
enum class EKind : uint8 { Action, Toggle, Radio, Submenu };

struct FItem
{
	FString Id;
	FString Label;
	EKind Kind = EKind::Action;
	/** A pinned direction, or None to take the next free slot. */
	EDir Dir = EDir::None;
	/** The NAME of the predicate the context answers; empty: always. */
	FString When;
	/** The NAME of the state a toggle or radio reads. */
	FString Read;
	/** The NAME of the command the item runs. */
	FString Write;
	/** What a radio stands for. */
	double Value = 0.0;
	/** A submenu's children, inline, and/or the NAME of a list the context
	 * produces when it unfolds. */
	TArray<FItem> Items;
	FString Children;
	/** Always in the stacked column, never the ring. */
	bool bOverflow = false;
};

struct FMenu
{
	FString Id;
	FString Label;
	TArray<FItem> Items;
};

/** What the model asks about the world. No side effects. */
class IContext
{
public:
	virtual ~IContext() = default;
	virtual bool Test(const FString& Name) const { return false; }
	/** The state behind a toggle's or radio's `Read`; unset reads as
	 * unchecked. */
	virtual TOptional<double> Read(const FString& Name) const { return {}; }
	virtual FString Label(const FString& ItemId, const FString& Default) const { return Default; }
	virtual TArray<FItem> Children(const FString& Name) const { return {}; }
};

// -- Resolution and layout ---------------------------------------------------
enum class EMode : uint8 { Flick, Click };

struct FResolvedItem
{
	FItem Item;
	FString Label;
	bool bEnabled = true;
	/** Unset: no check mark at all; otherwise checked or not. */
	TOptional<bool> Checked;
	/** The slot it took, or None in the overflow column. */
	EDir Direction = EDir::None;
	bool IsSubmenu() const { return Item.Kind == EKind::Submenu; }
};

struct FResolvedMenu
{
	FMenu Source;
	EMode Mode = EMode::Flick;
	/** Indexed by EDir (0..7); empty slots have no value. */
	TOptional<FResolvedItem> Slots[8];
	TArray<FResolvedItem> Overflow;

	const FResolvedItem* Slot(EDir Dir) const;
	/** Any drawn item by id, overflow included. */
	const FResolvedItem* Find(const FString& Id) const;
};

/** Pinned items claim their slots first; the rest fill in FillOrder; what
 * does not fit (or says bOverflow) goes to the column. */
void Layout(TArray<FResolvedItem>& Items, FResolvedMenu& Out);
/** The menu for THIS selection: an item whose `When` says no is hidden
 * mid-flick and greyed out once the menu is up to be read. */
FResolvedMenu Resolve(const FMenu& Menu, const IContext& Context, EMode Mode);
FResolvedMenu ResolveChild(const FItem& Item, const IContext& Context, EMode Mode);

// -- The gesture -------------------------------------------------------------
enum class EEmit : uint8 { Idle, Show, Highlight, Open, Confirm, Cancel };

struct FEmission
{
	EEmit Kind = EEmit::Idle;
	/** On Confirm: the item to run. On Highlight: the lit one, if any. */
	TOptional<FResolvedItem> Item;
	/** On Confirm: whether anything was ever drawn. */
	bool bShown = false;
};

enum class EState : uint8 { Idle, Armed, Stroke, Click };

class FGesture
{
public:
	FGesture(const FMenu& InMenu, const IContext& InContext);

	EState GetState() const { return State; }
	EMode GetMode() const { return Mode; }
	bool IsShown() const { return bShown; }
	const FResolvedMenu& GetMenu() const { return Resolved; }
	FVector2f GetCentre() const { return Centre; }
	/** Where each menu above the current one is centred, root first. */
	TArray<FVector2f> ParentCentres() const;
	/** The depth a point goes back to (the nearest parent's circle), or
	 * INDEX_NONE. */
	int32 BackTarget(const FVector2f& Point) const;

	TArray<FEmission> Press(const FVector2f& Point, double Ms);
	TArray<FEmission> Move(const FVector2f& Point, double Ms);
	TArray<FEmission> Tick(double Ms);
	/** `ItemId` names the drawn label or column row under the release, which
	 * outranks the direction. */
	TArray<FEmission> Release(const FVector2f& Point, double Ms, const FString& ItemId = FString());
	/** Confirms an item by id (an overflow row, or a label clicked). */
	TArray<FEmission> Choose(const FString& ItemId, double Ms);
	TArray<FEmission> Cancel();

private:
	void Reset();
	double Since(double Now) const { return Now - PressTime; }
	TArray<FEmission> ReleaseOn(const FResolvedItem& Item, double Now);
	TArray<FEmission> MaybeShow(double Now);
	TArray<FEmission> Aim(double Now);
	TArray<FEmission> MaybeUnfold(double Now);
	TArray<FEmission> Unfold(const FResolvedItem& Item, const FVector2f& At, double Now);
	TArray<FEmission> Back(int32 Index, double Now);
	TArray<FEmission> EnterClickMode(double Now);
	TArray<FEmission> ClickPress(const FVector2f& Point, double Now);
	TArray<FEmission> Confirm(const FResolvedItem& Item);
	TArray<FEmission> CancelWith();

	FMenu Menu;
	const IContext& Context;
	EState State = EState::Idle;
	EMode Mode = EMode::Flick;
	FResolvedMenu Resolved;
	TArray<TPair<FResolvedMenu, FVector2f>> Stack;
	FVector2f Centre = FVector2f::ZeroVector;
	FVector2f Point = FVector2f::ZeroVector;
	double PressTime = 0.0;
	EDir Direction = EDir::None;
	bool bShown = false;
	EDir Highlighted = EDir::None;
	bool bHighlightReported = false;
	double DwellFrom = 0.0;
};

// -- The built-in menus ------------------------------------------------------
/** The Selection menu: the viewport's right-click menu for rig controls.
 * Every ring item is pinned so the hand learns where each one is. */
const FMenu& SelectionMenu();
} // namespace RigExecMarking
