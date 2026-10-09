#include "RigExecPickerModel.h"

#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/FileHelper.h"
#include "RigExecActor.h"
#include "RigExecComponent.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Rigs/RigHierarchy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "RigExecPicker"

namespace
{
// The IK/FK dial is the blend weight, 0 FK and 1 IK; in between both halves
// stay drawn so either can be grabbed during a hand-over.
constexpr float GModeEnd = 0.001f;
// usdview draws fonts in pixels; a Slate font size is in points at 96 dpi.
constexpr float GPointsPerPixel = 0.75f;
constexpr float GMinFontPixels = 5.0f;

FColor
ReadColor(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FColor Default)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() < 3)
	{
		return Default;
	}
	auto Channel = [&](int32 I, uint8 Fallback) {
		return Values->IsValidIndex(I) ? uint8(FMath::Clamp((*Values)[I]->AsNumber(), 0.0, 255.0)) : Fallback;
	};
	return FColor(Channel(0, 0), Channel(1, 0), Channel(2, 0), Channel(3, 255));
}

FLinearColor
Tint(FColor Color, float Opacity)
{
	// The picker's colours are sRGB; Slate tints are linear.
	FLinearColor Linear(Color);
	Linear.A *= Opacity;
	return Linear;
}

FColor
VertexColor(FColor Color, float Opacity)
{
	// Slate vertices carry linear values packed to bytes.
	return Tint(Color, Opacity).ToFColor(false);
}

TArray<FVector2f>
ReadPoints(const TArray<TSharedPtr<FJsonValue>>& Flat)
{
	TArray<FVector2f> Points;
	for (int32 I = 0; I + 1 < Flat.Num(); I += 2)
	{
		Points.Emplace(float(Flat[I]->AsNumber()), float(Flat[I + 1]->AsNumber()));
	}
	return Points;
}
} // namespace

FRigExecPickerModel&
FRigExecPickerModel::Get()
{
	static FRigExecPickerModel Model;
	return Model;
}

void
FRigExecPickerModel::Bind()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	ARigExecActor* Found = Actor.Get();
	if (!Found || Found->GetWorld() != World)
	{
		Found = nullptr;
		for (TActorIterator<ARigExecActor> It(World); World && It; ++It)
		{
			Found = *It;
			break;
		}
	}
	const FString File = Found && Found->Rig ? Found->Rig->PickerFilePath() : FString();
	if (Found == Actor.Get() && File == LoadedFile && (Panels.Num() > 0 || File.IsEmpty()))
	{
		return;
	}
	Actor = Found;
	LoadedFile = File;
	Panels.Reset();
	PickerCount = 0;
	++Generation;
	if (!Found)
	{
		StatusText = TEXT("No RigExec character in the level.");
	}
	else if (File.IsEmpty())
	{
		StatusText = FString::Printf(TEXT("%s has no picker file beside its controls file."), *Found->GetActorLabel());
	}
	else if (!Rig())
	{
		// The Control Rig instance appears once the component has run;
		// try again on the next refresh.
		LoadedFile.Reset();
		StatusText = TEXT("Waiting for the character's Control Rig.");
	}
	else if (LoadPicker(File))
	{
		StatusText = Found->GetActorLabel();
	}
}

UControlRig*
FRigExecPickerModel::Rig() const
{
	const ARigExecActor* Found = Actor.Get();
	return Found && Found->Controls ? Found->Controls->GetControlRig() : nullptr;
}

bool
FRigExecPickerModel::LoadPicker(const FString& File)
{
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *File) ||
	    !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		StatusText = FString::Printf(TEXT("Cannot read %s"), *File);
		return false;
	}
	const URigExecComponent* Component = Actor->Rig;
	URigHierarchy* Hierarchy = Rig()->GetHierarchy();
	auto Control = [&](const FString& Path) {
		const FName Name = Component->FindControlElement(Path);
		return !Name.IsNone() && Hierarchy->Contains(FRigElementKey(Name, ERigElementType::Control)) ? Name : NAME_None;
	};
	auto Channel = [&](const FString& Path) {
		const FName Name = Component->FindChannelElement(Path);
		return !Name.IsNone() && Hierarchy->Contains(FRigElementKey(Name, ERigElementType::Control)) ? Name : NAME_None;
	};
	int32 Live = 0;
	for (const TSharedPtr<FJsonValue>& PickerValue : Root->GetArrayField(TEXT("pickers")))
	{
		const TSharedPtr<FJsonObject> PickerObject = PickerValue->AsObject();
		FString PickerName;
		PickerObject->TryGetStringField(TEXT("name"), PickerName);
		++PickerCount;
		for (const TSharedPtr<FJsonValue>& PanelValue : PickerObject->GetArrayField(TEXT("panels")))
		{
			const TSharedPtr<FJsonObject> PanelObject = PanelValue->AsObject();
			FRigExecPickerPanel& Panel = Panels.AddDefaulted_GetRef();
			Panel.Picker = PickerName;
			Panel.Label = PanelObject->GetStringField(TEXT("label"));
			const TArray<TSharedPtr<FJsonValue>>& Size = PanelObject->GetArrayField(TEXT("size"));
			Panel.Size = FVector2f(float(Size[0]->AsNumber()), float(Size[1]->AsNumber()));
			Panel.Fill = ReadColor(PanelObject, TEXT("fill"), FColor(68, 68, 68));
			for (const TSharedPtr<FJsonValue>& ButtonValue : PanelObject->GetArrayField(TEXT("buttons")))
			{
				const TSharedPtr<FJsonObject> B = ButtonValue->AsObject();
				FRigExecPickerButton& Button = Panel.Buttons.AddDefaulted_GetRef();
				Button.Id = B->GetStringField(TEXT("id"));
				const TArray<TSharedPtr<FJsonValue>>& Box = B->GetArrayField(TEXT("box"));
				const FVector2f Min(float(Box[0]->AsNumber()), float(Box[1]->AsNumber()));
				Button.Box = FBox2f(Min, Min + FVector2f(float(Box[2]->AsNumber()), float(Box[3]->AsNumber())));
				Button.Vertices = ReadPoints(B->GetArrayField(TEXT("vertices")));
				for (const TSharedPtr<FJsonValue>& I : B->GetArrayField(TEXT("triangles")))
				{
					Button.Triangles.Add(uint32(I->AsNumber()));
				}
				for (const TSharedPtr<FJsonValue>& Outline : B->GetArrayField(TEXT("outlines")))
				{
					TArray<FVector2f> Points = ReadPoints(Outline->AsArray());
					if (Points.Num() > 1)
					{
						Button.Outlines.Add(MoveTemp(Points));
					}
				}
				Button.Fill = ReadColor(B, TEXT("fill"), FColor(128, 128, 128));
				Button.Stroke = ReadColor(B, TEXT("stroke"), FColor(0, 0, 0, 200));
				Button.TextColor = ReadColor(B, TEXT("textColor"), FColor::Black);
				Button.ValueColor = ReadColor(B, TEXT("valueColor"), FColor(235, 235, 235));
				Button.StrokeWidth = float(B->GetNumberField(TEXT("strokeWidth")));
				Button.Text = B->GetStringField(TEXT("text"));
				Button.FontSize = float(B->GetNumberField(TEXT("fontSize")));
				Button.bBold = B->GetBoolField(TEXT("bold"));
				const FString Align = B->GetStringField(TEXT("align"));
				Button.Align = Align == TEXT("left") ? -1 : Align == TEXT("right") ? 1 : 0;
				Button.bDecoration = B->GetBoolField(TEXT("decoration"));
				Button.Mirror = B->GetStringField(TEXT("mirror"));
				B->TryGetStringField(TEXT("command"), Button.Command);
				FString Dial;
				if (B->TryGetStringField(TEXT("dial"), Dial))
				{
					Button.DialChannel = Channel(Dial);
					Button.Mode = B->GetStringField(TEXT("mode"));
				}

				bool bResolved = true;
				const TArray<TSharedPtr<FJsonValue>>& Targets = B->GetArrayField(TEXT("targets"));
				for (const TSharedPtr<FJsonValue>& Target : Targets)
				{
					const FName Name = Control(Target->AsString());
					bResolved &= !Name.IsNone();
					Button.Targets.AddUnique(Name);
				}
				const TSharedPtr<FJsonObject>* Attribute = nullptr;
				if (!Button.Command.IsEmpty())
				{
					Button.bLive = true;
				}
				else if (B->TryGetObjectField(TEXT("attribute"), Attribute))
				{
					const FString Path = (*Attribute)->GetStringField(TEXT("path"));
					Button.Channel = Channel(Path);
					Button.ChannelPath = Path;
					for (const TSharedPtr<FJsonValue>& Label : (*Attribute)->GetArrayField(TEXT("labels")))
					{
						Button.Labels.Add(Label->AsString());
					}
					Button.bInvert = (*Attribute)->GetBoolField(TEXT("invert"));
					// A space dial sits on the control it switches.
					FString Owner, Attr;
					if (Path.Split(TEXT("."), &Owner, &Attr) && Attr == TEXT("avars:space"))
					{
						Button.SwitchedControl = Control(Owner);
					}
					Button.bLive = !Button.Channel.IsNone();
				}
				else
				{
					Button.bLive = Targets.Num() > 0 && bResolved;
				}
				if (!bResolved)
				{
					Button.Targets.Remove(NAME_None);
				}
				Live += (Button.bLive && !Button.bDecoration) ? 1 : 0;
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("RigExec picker: %d panels, %d live buttons from %s"), Panels.Num(), Live, *File);
	return Panels.Num() > 0;
}

const FRigExecPickerPanel*
FRigExecPickerModel::GetPanel(int32 Panel) const
{
	return Panels.IsValidIndex(Panel) ? &Panels[Panel] : nullptr;
}

float
FRigExecPickerModel::ChannelValue(FName Channel) const
{
	UControlRig* ControlRig = Rig();
	URigHierarchy* Hierarchy = ControlRig ? ControlRig->GetHierarchy() : nullptr;
	const FRigElementKey Key(Channel, ERigElementType::Control);
	return Hierarchy && Hierarchy->Contains(Key) ? Hierarchy->GetControlValue(Key).Get<float>() : 0.0f;
}

TArray<int32>
FRigExecPickerModel::Visible(int32 PanelIndex, bool bEitherMode) const
{
	TArray<int32> Out;
	const FRigExecPickerPanel* Panel = GetPanel(PanelIndex);
	if (!Panel)
	{
		return Out;
	}
	for (int32 Index = 0; Index < Panel->Buttons.Num(); ++Index)
	{
		const FRigExecPickerButton& Button = Panel->Buttons[Index];
		if (!Button.bDecoration && !Button.bLive)
		{
			continue;
		}
		if (!bEitherMode && !Button.Mode.IsEmpty() && !Button.DialChannel.IsNone())
		{
			const float Dial = ChannelValue(Button.DialChannel);
			const TCHAR* Mode = Dial >= 1.0f - GModeEnd ? TEXT("ik") : Dial <= GModeEnd ? TEXT("fk") : nullptr;
			if (Mode && Button.Mode != Mode)
			{
				continue;
			}
		}
		Out.Add(Index);
	}
	return Out;
}

int32
FRigExecPickerModel::HitAt(int32 PanelIndex, const FVector2f& At) const
{
	const TArray<int32> Drawn = Visible(PanelIndex);
	for (int32 K = Drawn.Num() - 1; K >= 0; --K)
	{
		const FRigExecPickerButton& Button = Panels[PanelIndex].Buttons[Drawn[K]];
		if (!Button.bDecoration && Button.bLive && Button.Box.IsInsideOrOn(At))
		{
			return Drawn[K];
		}
	}
	return INDEX_NONE;
}

TArray<int32>
FRigExecPickerModel::Within(int32 PanelIndex, const FBox2f& Band) const
{
	TArray<int32> Out;
	for (int32 Index : Visible(PanelIndex))
	{
		const FRigExecPickerButton& Button = Panels[PanelIndex].Buttons[Index];
		if (!Button.bDecoration && Button.bLive && Button.Channel.IsNone() && Button.Command.IsEmpty() &&
		    Button.Box.Intersect(Band))
		{
			Out.Add(Index);
		}
	}
	return Out;
}

bool
FRigExecPickerModel::IsSelected(const FRigExecPickerButton& Button) const
{
	UControlRig* ControlRig = Rig();
	if (!ControlRig || Button.Targets.Num() == 0)
	{
		return false;
	}
	for (const FName& Target : Button.Targets)
	{
		if (!ControlRig->IsControlSelected(Target))
		{
			return false;
		}
	}
	return true;
}

FString
FRigExecPickerModel::ValueLabel(const FRigExecPickerButton& Button) const
{
	if (Button.Channel.IsNone() || Button.Labels.Num() == 0)
	{
		return FString();
	}
	int32 Index = FMath::RoundToInt(ChannelValue(Button.Channel));
	if (Button.bInvert)
	{
		Index = Button.Labels.Num() - 1 - Index;
	}
	return Button.Labels.IsValidIndex(Index) ? Button.Labels[Index] : FString();
}

bool
FRigExecPickerModel::ControlsShown() const
{
	UControlRig* ControlRig = Rig();
	return ControlRig && ControlRig->GetControlsVisible();
}

void
FRigExecPickerModel::Pick(int32 PanelIndex, const TArray<int32>& Picked, int32 Mode, bool bMirror,
                          const FVector2D& ScreenAt, const TSharedRef<SWidget>& MenuParent)
{
	UControlRig* ControlRig = Rig();
	const FRigExecPickerPanel* Panel = GetPanel(PanelIndex);
	if (!ControlRig || !Panel)
	{
		return;
	}
	// A command or an attribute button acts, on a plain click of one.
	if (Picked.Num() == 1 && Mode == 0)
	{
		const FRigExecPickerButton& Button = Panel->Buttons[Picked[0]];
		if (Button.Command == TEXT("zero_ctrls"))
		{
			ZeroControls();
			return;
		}
		if (Button.Command == TEXT("ctrl_vis"))
		{
			ToggleControls();
			return;
		}
		if (!Button.Channel.IsNone())
		{
			Switch(Button, ScreenAt, MenuParent);
			return;
		}
	}
	TArray<FName> Wanted;
	for (int32 Index : Picked)
	{
		const FRigExecPickerButton& Button = Panel->Buttons[Index];
		for (const FName& Target : Button.Targets)
		{
			Wanted.AddUnique(Target);
		}
		if (bMirror && !Button.Mirror.IsEmpty())
		{
			for (const FRigExecPickerButton& Other : Panel->Buttons)
			{
				if (Other.Id == Button.Mirror && Other.bLive)
				{
					for (const FName& Target : Other.Targets)
					{
						Wanted.AddUnique(Target);
					}
				}
			}
		}
	}
	const FScopedTransaction Transaction(LOCTEXT("PickerSelect", "Select Controls"));
	if (Mode == 0)
	{
		ControlRig->ClearControlSelection(true);
	}
	for (const FName& Target : Wanted)
	{
		const bool bSelect = Mode == 0 || (Mode == 1 && !ControlRig->IsControlSelected(Target));
		ControlRig->SelectControl(Target, bSelect, true);
	}
}

void
FRigExecPickerModel::SetChannel(FName Channel, float Value, bool bMatch, FName Switched)
{
	UControlRig* ControlRig = Rig();
	URigExecComponent* Component = Actor.IsValid() ? Actor->Rig.Get() : nullptr;
	if (!ControlRig || !Component)
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("PickerSwitch", "Switch Control"));
	const FRigControlModifiedContext Context(EControlRigSetKey::DoNotCare);
	const bool bHold = bMatch && !Switched.IsNone();
	const FTransform Before = bHold ? ControlRig->GetControlGlobalTransform(Switched) : FTransform::Identity;
	ControlRig->SetControlValue<float>(Channel, Value, true, Context, true);
	if (!bHold)
	{
		return;
	}
	// Keep the switched control where it was: evaluate in the new space
	// (which re-places the controls), put the control back on its old
	// frame, and evaluate again for whatever it carries.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		Component->PullControls();
		ControlRig->SetControlGlobalTransform(Switched, Before, true, Context, true);
	}
	Component->PullControls();
}

void
FRigExecPickerModel::Switch(const FRigExecPickerButton& Button, const FVector2D& ScreenAt,
                            const TSharedRef<SWidget>& MenuParent)
{
	const int32 Count = Button.Labels.Num();
	auto ValueOf = [&Button, Count](int32 Index) { return float(Button.bInvert ? Count - 1 - Index : Index); };
	if (Count > 2)
	{
		// A space switch offers its spaces; the control stays put.
		FMenuBuilder Menu(true, nullptr);
		const FString Current = ValueLabel(Button);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FName Channel = Button.Channel;
			const FName Switched = Button.SwitchedControl;
			const float Value = ValueOf(Index);
			Menu.AddMenuEntry(
			    FText::FromString(Button.Labels[Index]), FText::GetEmpty(), FSlateIcon(),
			    FUIAction(FExecuteAction::CreateLambda([this, Channel, Switched, Value]() {
				              SetChannel(Channel, Value, true, Switched);
			              }),
			              FCanExecuteAction(),
			              FIsActionChecked::CreateLambda([Current, Label = Button.Labels[Index]]() { return Current == Label; })),
			    NAME_None, EUserInterfaceActionType::RadioButton);
		}
		FSlateApplication::Get().PushMenu(MenuParent, FWidgetPath(), Menu.MakeWidget(), ScreenAt,
		                                  FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		return;
	}
	if (Count == 0)
	{
		return;
	}
	// An IK/FK switch matches the incoming half to the limb first.
	URigExecComponent* Component = Actor.IsValid() ? Actor->Rig.Get() : nullptr;
	if (Component && Component->IsLimbSwitch(Button.ChannelPath))
	{
		const FScopedTransaction Transaction(LOCTEXT("PickerMatch", "Switch Limb"));
		if (Component->SwitchLimb(Button.ChannelPath))
		{
			StatusText = FString::Printf(TEXT("%s, matched"), *ValueLabel(Button));
			return;
		}
		StatusText = TEXT("IK/FK match failed; switched without matching.");
	}
	const FString Here = ValueLabel(Button);
	const int32 Next = (FMath::Max(Button.Labels.IndexOfByKey(Here), 0) + 1) % Count;
	SetChannel(Button.Channel, ValueOf(Next), !Button.SwitchedControl.IsNone(), Button.SwitchedControl);
}

void
FRigExecPickerModel::ZeroControls()
{
	UControlRig* ControlRig = Rig();
	URigHierarchy* Hierarchy = ControlRig ? ControlRig->GetHierarchy() : nullptr;
	if (!Hierarchy)
	{
		return;
	}
	// The selected controls and their channels, or the whole rig when
	// nothing is selected: each back to its initial value.
	const TArray<FName> Selected = ControlRig->CurrentControlSelection();
	const FScopedTransaction Transaction(LOCTEXT("PickerZero", "Zero Controls"));
	const FRigControlModifiedContext Context(EControlRigSetKey::DoNotCare);
	int32 Zeroed = 0;
	for (FRigControlElement* Control : Hierarchy->GetControls())
	{
		if (Control->Settings.AnimationType == ERigControlAnimationType::VisualCue)
		{
			continue;
		}
		const FRigElementKey Parent = Hierarchy->GetFirstParent(Control->GetKey());
		if (Selected.Num() > 0 && !Selected.Contains(Control->GetFName()) &&
		    !(Control->IsAnimationChannel() && Selected.Contains(Parent.Name)))
		{
			continue;
		}
		ControlRig->SetControlValueImpl(Control->GetFName(),
		                                Hierarchy->GetControlValue(Control, ERigControlValueType::Initial), true,
		                                Context, true);
		++Zeroed;
	}
	StatusText = FString::Printf(TEXT("Zeroed %d controls%s"), Zeroed,
	                             Selected.Num() ? TEXT("") : TEXT(" (nothing selected: the whole rig)"));
}

void
FRigExecPickerModel::ToggleControls()
{
	if (UControlRig* ControlRig = Rig())
	{
		ControlRig->SetControlsVisible(!ControlRig->GetControlsVisible());
	}
}

void
FRigExecPickerModel::PaintButton(const FRigExecPickerButton& Source, bool bHovered, const FGeometry& Geometry,
                                 const FVector2f& Offset, float S, float Opacity, FSlateWindowElementList& Out,
                                 int32 Layer) const
{
	FRigExecPickerButton Button = Source;
	if (bHovered && Button.bLive && !Button.bDecoration)
	{
		auto Lift = [](uint8 C) { return uint8(FMath::Min(int32(C) * 5 / 4, 255)); };
		Button.Fill = FColor(Lift(Button.Fill.R), Lift(Button.Fill.G), Lift(Button.Fill.B), Button.Fill.A);
	}
	auto Map = [&Offset, S](const FVector2f& P) { return Offset + P * S; };
	const FSlateBrush* White = FAppStyle::GetBrush("WhiteBrush");
	const FSlateRenderTransform& Render = Geometry.GetAccumulatedRenderTransform();
	if (Button.Triangles.Num() > 0)
	{
		TArray<FSlateVertex> Verts;
		Verts.Reserve(Button.Vertices.Num());
		for (const FVector2f& P : Button.Vertices)
		{
			Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Map(P), FVector2f(0.5f, 0.5f),
			                                                             VertexColor(Button.Fill, Opacity)));
		}
		TArray<SlateIndex> Indices;
		Indices.Reserve(Button.Triangles.Num());
		for (uint32 I : Button.Triangles)
		{
			Indices.Add(SlateIndex(I));
		}
		const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*White);
		FSlateDrawElement::MakeCustomVerts(Out, Layer, Handle, Verts, Indices, nullptr, 0, 0);
	}

	const bool bSelected = IsSelected(Button);
	FLinearColor Stroke = bSelected ? FLinearColor::White : Tint(Button.Stroke, 1.0f);
	Stroke.A *= Opacity;
	const float Thickness = bSelected ? 2.0f : FMath::Max(Button.StrokeWidth, 0.5f);
	for (const TArray<FVector2f>& Outline : Button.Outlines)
	{
		TArray<FVector2f> Points;
		Points.Reserve(Outline.Num() + 1);
		for (const FVector2f& P : Outline)
		{
			Points.Add(Map(P));
		}
		Points.Add(Map(Outline[0]));
		FSlateDrawElement::MakeLines(Out, Layer + 1, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None,
		                             Stroke, true, Thickness);
	}

	FVector2f Origin = Button.Box.Min;
	FVector2f Size = Button.Box.GetSize();
	if (Button.Command == TEXT("ctrl_vis"))
	{
		// A toggle draws a checkbox ticked while the controls show.
		const float Side = FMath::Min(Size.Y - 4.0f, 11.0f);
		const FVector2f Corner(Origin.X + 3.0f, Origin.Y + (Size.Y - Side) * 0.5f);
		FSlateDrawElement::MakeBox(Out, Layer + 1,
		                           Geometry.ToPaintGeometry(FVector2f(Side, Side) * S, FSlateLayoutTransform(Map(Corner))),
		                           White, ESlateDrawEffect::None, Tint(FColor(58, 58, 58), Opacity));
		if (ControlsShown())
		{
			TArray<FVector2f> Tick = {Map(Corner + FVector2f(0.20f, 0.52f) * Side),
			                          Map(Corner + FVector2f(0.44f, 0.76f) * Side),
			                          Map(Corner + FVector2f(0.82f, 0.24f) * Side)};
			FSlateDrawElement::MakeLines(Out, Layer + 2, Geometry.ToPaintGeometry(), Tick, ESlateDrawEffect::None,
			                             Tint(FColor(120, 200, 255), Opacity), true, 1.8f * S);
		}
		Origin.X += Side + 5.0f;
		Size.X -= Side + 5.0f;
	}

	const FString Value = ValueLabel(Button);
	if (Button.Text.IsEmpty() && Value.IsEmpty())
	{
		return;
	}
	// Fit the label and the value together, as the usdview picker does.
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(Button.bBold ? "Bold" : "Regular", 8);
	float Pixels = FMath::Max(FMath::RoundToFloat(Button.FontSize), GMinFontPixels);
	const float Room = Size.X - 4.0f - (Value.IsEmpty() ? 0.0f : 14.0f);
	const FString Both = Button.Text.IsEmpty() ? Value : Value.IsEmpty() ? Button.Text : Button.Text + TEXT(" ") + Value;
	for (int32 Step = 0; Step < 6; ++Step)
	{
		Font.Size = Pixels * GPointsPerPixel;
		if (Measure->Measure(Both, Font).X <= Room || Pixels <= GMinFontPixels)
		{
			break;
		}
		Pixels -= 1.0f;
	}
	auto Text = [&](const FString& String, float Left, float Right, int32 Align, FColor Color) {
		const FVector2f Extent = FVector2f(Measure->Measure(String, Font));
		const float X = Align < 0 ? Left : Align > 0 ? Right - Extent.X : (Left + Right - Extent.X) * 0.5f;
		const FVector2f At(X, Origin.Y + (Size.Y - Extent.Y) * 0.5f);
		FSlateDrawElement::MakeText(Out, Layer + 2, Geometry.ToPaintGeometry(Extent, FSlateLayoutTransform(S, Map(At))),
		                            String, Font, ESlateDrawEffect::None, Tint(Color, Opacity));
	};
	const float Left = Origin.X + 2.0f;
	const float Right = Origin.X + Size.X - 2.0f;
	const int32 Align = Button.Command == TEXT("ctrl_vis") ? -1 : Button.Align;
	if (Value.IsEmpty())
	{
		Text(Button.Text, Left, Right, Align, Button.TextColor);
		return;
	}
	// The value sits on the side away from the label, with a caret.
	const float Want = FMath::Min(float(Measure->Measure(Value, Font).X) + 14.0f, Right - Left);
	const bool bValueLeft = Align > 0;
	const float ValueLo = Button.Text.IsEmpty() ? Left : bValueLeft ? Left : Right - Want;
	const float ValueHi = Button.Text.IsEmpty() ? Right : bValueLeft ? Left + Want : Right;
	if (!Button.Text.IsEmpty())
	{
		Text(Button.Text, bValueLeft ? ValueHi : Left, bValueLeft ? Right : ValueLo, Align, Button.TextColor);
	}
	Text(Value, bValueLeft ? ValueLo + 12.0f : ValueLo, bValueLeft ? ValueHi : ValueHi - 12.0f,
	     bValueLeft ? -1 : 1, Button.ValueColor);
	const float CaretX = bValueLeft ? ValueLo + 5.0f : ValueHi - 7.0f;
	const float Mid = Origin.Y + Size.Y * 0.5f;
	const FVector2f Caret[3] = {FVector2f(CaretX - 3.2f, Mid - 1.6f), FVector2f(CaretX + 3.2f, Mid - 1.6f),
	                            FVector2f(CaretX, Mid + 2.2f)};
	TArray<FSlateVertex> Verts;
	for (const FVector2f& P : Caret)
	{
		Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Map(P), FVector2f(0.5f, 0.5f),
		                                                             VertexColor(Button.ValueColor, Opacity)));
	}
	const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*White);
	FSlateDrawElement::MakeCustomVerts(Out, Layer + 3, Handle, Verts, TArray<SlateIndex>{0, 1, 2}, nullptr, 0, 0);
}

#undef LOCTEXT_NAMESPACE
