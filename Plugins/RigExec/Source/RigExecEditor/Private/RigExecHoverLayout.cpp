#include "RigExecHoverLayout.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace RigExecHover
{
namespace
{
FString
LayoutFile()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExec"), TEXT("HoverPicker.json"));
}
} // namespace

FString
TabKey(const FString& Picker, const FString& Panel)
{
	return Picker + TEXT("/") + Panel;
}

float
FitScale(float ContentHeight, float ViewHeight)
{
	if (ContentHeight <= 0.0f || ViewHeight <= 0.0f)
	{
		return 1.0f;
	}
	return FMath::Clamp(FMath::Min(1.0f, FitFraction * ViewHeight / ContentHeight), MinScale, MaxScale);
}

FVector2f
KnobCentre(const FVector2f& ViewSize)
{
	return FVector2f(ViewSize.X - KnobInset - KnobRadius, ViewSize.Y - KnobBottomClearance - KnobRadius);
}

bool
HitsKnob(const FVector2f& ViewSize, const FVector2f& Point)
{
	return FVector2f::Distance(Point, KnobCentre(ViewSize)) <= KnobRadius + HandleSlop;
}

FVector2f
FTabLayout::ButtonsCorner() const
{
	return FVector2f(X - HandleRadius, Y + HandleRadius + HandleGap);
}

FVector2f
FTabLayout::ToScreen(const FVector2f& Origin, const FVector2f& Point) const
{
	return ButtonsCorner() + (Point - Origin) * Scale;
}

FVector2f
FTabLayout::ToPanel(const FVector2f& Origin, const FVector2f& Point) const
{
	return Origin + (Point - ButtonsCorner()) / Scale;
}

bool
FTabLayout::HitsHandle(const FVector2f& Point) const
{
	return FVector2f::Distance(Point, FVector2f(X, Y)) <= HandleRadius + HandleSlop;
}

void
FTabLayout::Moved(const FVector2f& Delta)
{
	X += Delta.X;
	Y += Delta.Y;
}

void
FTabLayout::Scaled(float StartScale, const FVector2f& Delta)
{
	// The exponent is bounded first: the result clamps anyway, and a drag
	// far off screen must not overflow on the way there.
	const float Power = FMath::Clamp(ScaleRate * (Delta.X - Delta.Y), -50.0f, 50.0f);
	Scale = FMath::Clamp(StartScale * FMath::Exp(Power), MinScale, MaxScale);
}

void
FTabLayout::Faded(float StartOpacity, float DeltaX)
{
	Opacity = FMath::Clamp(StartOpacity + DeltaX * OpacityRate, MinOpacity, MaxOpacity);
}

void
FLayout::Ensure(const TArray<FString>& Keys, const TMap<FString, float>& FitScales)
{
	float NextX = FirstHandleX;
	bool bPlaced = false;
	for (const TPair<FString, FTabLayout>& Tab : Tabs)
	{
		if (FMath::Abs(Tab.Value.Y - FirstHandleY) < 1e-4f)
		{
			NextX = bPlaced ? FMath::Max(NextX, Tab.Value.X + HandleSpacing) : Tab.Value.X + HandleSpacing;
			bPlaced = true;
		}
	}
	// Adjacent handles would open their groups on top of each other, so a
	// fresh layout opens only its first tab; the rest wait as handles in
	// the row.
	bool bOpened = Tabs.Num() > 0;
	for (const FString& Key : Keys)
	{
		if (Tabs.Contains(Key))
		{
			continue;
		}
		FTabLayout& Tab = Tabs.Add(Key);
		Tab.X = NextX;
		Tab.Y = FirstHandleY;
		const float* Fit = FitScales.Find(Key);
		Tab.Scale = FMath::Clamp(Fit ? *Fit : 1.0f, MinScale, MaxScale);
		Tab.bCollapsed = bOpened;
		bOpened = true;
		NextX += HandleSpacing;
	}
}

void
FLayout::Faded(float StartOpacity, float DeltaX)
{
	Opacity = FMath::Clamp(StartOpacity + DeltaX * OpacityRate, MinOpacity, MaxOpacity);
}

float
FLayout::TabOpacity(const FString& Key) const
{
	const FTabLayout* Tab = Tabs.Find(Key);
	return (Tab ? Tab->Opacity : 1.0f) * Opacity;
}

FString
FLayout::ToJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("enabled"), bEnabled);
	Root->SetNumberField(TEXT("opacity"), Opacity);
	TSharedRef<FJsonObject> TabsObject = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Tabs.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const FTabLayout& Tab = Tabs[Key];
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Tab.X);
		Object->SetNumberField(TEXT("y"), Tab.Y);
		Object->SetNumberField(TEXT("scale"), Tab.Scale);
		Object->SetNumberField(TEXT("opacity"), Tab.Opacity);
		Object->SetBoolField(TEXT("collapsed"), Tab.bCollapsed);
		TabsObject->SetObjectField(Key, Object);
	}
	Root->SetObjectField(TEXT("tabs"), TabsObject);
	FString Text;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
	return Text;
}

FLayout
FLayout::FromJson(const FString& Text)
{
	FLayout Layout;
	TSharedPtr<FJsonObject> Root;
	if (Text.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return Layout;
	}
	Root->TryGetBoolField(TEXT("enabled"), Layout.bEnabled);
	double Opacity = 1.0;
	if (Root->TryGetNumberField(TEXT("opacity"), Opacity))
	{
		Layout.Opacity = FMath::Clamp(float(Opacity), MinOpacity, MaxOpacity);
	}
	const TSharedPtr<FJsonObject>* TabsObject = nullptr;
	if (Root->TryGetObjectField(TEXT("tabs"), TabsObject))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*TabsObject)->Values)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Entry.Value.IsValid() || !Entry.Value->TryGetObject(Object))
			{
				continue;
			}
			FTabLayout Tab;
			double Value = 0.0;
			if ((*Object)->TryGetNumberField(TEXT("x"), Value)) { Tab.X = float(Value); }
			if ((*Object)->TryGetNumberField(TEXT("y"), Value)) { Tab.Y = float(Value); }
			if ((*Object)->TryGetNumberField(TEXT("scale"), Value)) { Tab.Scale = FMath::Clamp(float(Value), MinScale, MaxScale); }
			if ((*Object)->TryGetNumberField(TEXT("opacity"), Value)) { Tab.Opacity = FMath::Clamp(float(Value), MinOpacity, MaxOpacity); }
			(*Object)->TryGetBoolField(TEXT("collapsed"), Tab.bCollapsed);
			Layout.Tabs.Add(Entry.Key, Tab);
		}
	}
	return Layout;
}

FLayout
FLayout::Load()
{
	FString Text;
	FFileHelper::LoadFileToString(Text, *LayoutFile());
	return FromJson(Text);
}

void
FLayout::Save() const
{
	FFileHelper::SaveStringToFile(ToJson(), *LayoutFile());
}
} // namespace RigExecHover
