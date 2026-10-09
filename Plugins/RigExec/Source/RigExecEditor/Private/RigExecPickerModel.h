// A RigExec character's control picker, with no widget around it: the
// panels export_picker.py wrote beside the controls file, which buttons are
// live and shown, what a click selects or switches, and how a button is
// painted. The docked picker (SRigExecPicker) and the hover picker
// (FRigExecHoverPicker) both draw from this one model, so the two can never
// disagree about a button.
#pragma once

#include "CoreMinimal.h"

class ARigExecActor;
class FSlateWindowElementList;
class SWidget;
class UControlRig;
struct FGeometry;

struct FRigExecPickerButton
{
	FString Id;
	/** The unrotated box a click is tested against, panel units. */
	FBox2f Box;
	TArray<FVector2f> Vertices;
	TArray<uint32> Triangles;
	TArray<TArray<FVector2f>> Outlines;
	FColor Fill, Stroke, TextColor, ValueColor;
	float StrokeWidth = 1.0f;
	FString Text;
	float FontSize = 8.0f;
	bool bBold = false;
	/** -1 left, 0 centre, 1 right. */
	int32 Align = 0;
	bool bDecoration = false;
	/** Control Rig controls this button selects. */
	TArray<FName> Targets;
	/** Every target resolved (or the button acts rather than selects). */
	bool bLive = false;
	/** The channel an attribute button writes, its labels, and whether the
	 * label order runs against the value. */
	FName Channel;
	FString ChannelPath;
	TArray<FString> Labels;
	bool bInvert = false;
	/** The control a space switch keeps in place: the dial's own control. */
	FName SwitchedControl;
	FString Command;
	/** "ik" / "fk": drawn only while DialChannel is in that mode. */
	FString Mode;
	FName DialChannel;
	FString Mirror;
};

struct FRigExecPickerPanel
{
	/** The picker (character) the panel belongs to. */
	FString Picker;
	FString Label;
	FVector2f Size = FVector2f(400.0f, 600.0f);
	FColor Fill;
	TArray<FRigExecPickerButton> Buttons;
};

class FRigExecPickerModel
{
public:
	/** The one model every picker view shares. */
	static FRigExecPickerModel& Get();

	/** Finds the character and (re)loads its picker when it changed. */
	void Bind();
	/** Counts reloads: a view rebuilds what it derived from the panels when
	 * this moves. */
	int32 GetGeneration() const { return Generation; }
	const TArray<FRigExecPickerPanel>& GetPanels() const { return Panels; }
	const FRigExecPickerPanel* GetPanel(int32 Panel) const;
	int32 GetPickerCount() const { return PickerCount; }
	const FString& GetStatus() const { return StatusText; }
	ARigExecActor* GetActor() const { return Actor.Get(); }

	/** A panel's buttons drawn now, in painter order: live or decoration,
	 * and the half of each IK/FK pair the limb is in -- or both halves with
	 * `bEitherMode`. */
	TArray<int32> Visible(int32 Panel, bool bEitherMode = false) const;
	/** The topmost clickable button under a panel point, or INDEX_NONE. */
	int32 HitAt(int32 Panel, const FVector2f& At) const;
	/** The selectable buttons a marquee touches: buttons that act (switches
	 * and commands) are left out, since a marquee only ever selects. */
	TArray<int32> Within(int32 Panel, const FBox2f& Band) const;
	bool IsSelected(const FRigExecPickerButton& Button) const;
	/** The label of the value an attribute button's channel holds. */
	FString ValueLabel(const FRigExecPickerButton& Button) const;
	bool ControlsShown() const;

	/** A plain click on one button, or a marquee; `Mode` 0 replace,
	 * 1 toggle, 2 remove. A space switch's menu opens at `ScreenAt`, owned
	 * by `MenuParent`. */
	void Pick(int32 Panel, const TArray<int32>& Buttons, int32 Mode, bool bMirror, const FVector2D& ScreenAt,
	          const TSharedRef<SWidget>& MenuParent);

	/** Paints one button: panel point P lands at Offset + P * Scale in the
	 * geometry's local space. */
	void PaintButton(const FRigExecPickerButton& Button, bool bHovered, const FGeometry& Geometry,
	                 const FVector2f& Offset, float Scale, float Opacity, FSlateWindowElementList& Out,
	                 int32 Layer) const;

private:
	bool LoadPicker(const FString& File);
	UControlRig* Rig() const;
	float ChannelValue(FName Channel) const;
	void SetChannel(FName Channel, float Value, bool bMatch, FName Switched);
	void Switch(const FRigExecPickerButton& Button, const FVector2D& ScreenAt, const TSharedRef<SWidget>& MenuParent);
	void ZeroControls();
	void ToggleControls();

	TWeakObjectPtr<ARigExecActor> Actor;
	FString LoadedFile;
	TArray<FRigExecPickerPanel> Panels;
	int32 PickerCount = 0;
	int32 Generation = 0;
	FString StatusText;
};
