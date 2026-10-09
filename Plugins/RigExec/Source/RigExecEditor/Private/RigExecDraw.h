// Small Slate drawing helpers the viewport overlays share (the hover picker
// and the marking menu): filled discs and wedges, rings, and polylines, in
// a geometry's local space.
#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Geometry.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/AppStyle.h"

namespace RigExecDraw
{
/** A filled disc, or the wedge of one from twelve o'clock clockwise through
 * `Sweep` (0..1 of a turn). */
inline void
Disc(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& Centre, float Radius,
     const FLinearColor& Color, float Sweep = 1.0f)
{
	constexpr int32 Segments = 32;
	const int32 Count = FMath::Max(FMath::CeilToInt(Segments * Sweep), 1);
	const FSlateRenderTransform& Render = Geometry.GetAccumulatedRenderTransform();
	const FColor Packed = Color.ToFColor(false);
	TArray<FSlateVertex> Verts;
	TArray<SlateIndex> Indices;
	Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Centre, FVector2f(0.5f, 0.5f), Packed));
	for (int32 I = 0; I <= Count; ++I)
	{
		const float Angle = 2.0f * PI * Sweep * float(I) / float(Count);
		const FVector2f P = Centre + FVector2f(FMath::Sin(Angle), -FMath::Cos(Angle)) * Radius;
		Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, P, FVector2f(0.5f, 0.5f), Packed));
		if (I > 0)
		{
			Indices.Append({SlateIndex(0), SlateIndex(I), SlateIndex(I + 1)});
		}
	}
	const FSlateResourceHandle Handle =
		FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FAppStyle::GetBrush("WhiteBrush"));
	FSlateDrawElement::MakeCustomVerts(Out, Layer, Handle, Verts, Indices, nullptr, 0, 0);
}

inline void
Ring(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& Centre, float Radius,
     const FLinearColor& Color, float Thickness)
{
	constexpr int32 Segments = 32;
	TArray<FVector2f> Points;
	for (int32 I = 0; I <= Segments; ++I)
	{
		const float Angle = 2.0f * PI * float(I) / float(Segments);
		Points.Add(Centre + FVector2f(FMath::Sin(Angle), -FMath::Cos(Angle)) * Radius);
	}
	FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true,
	                             Thickness);
}

inline void
Lines(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const TArray<FVector2f>& Points,
      const FLinearColor& Color, float Thickness)
{
	// A line with no length makes a batch with no indices, which Slate's
	// renderer asserts on (a guide from the centre to a cursor still on it).
	float Length = 0.0f;
	for (int32 I = 1; I < Points.Num(); ++I)
	{
		Length += FVector2f::Distance(Points[I - 1], Points[I]);
	}
	if (Length < 0.5f)
	{
		return;
	}
	FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true,
	                             Thickness);
}

/** A dashed segment. */
inline void
Dashed(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& A, const FVector2f& B,
       const FLinearColor& Color, float Thickness, float Dash = 6.0f, float Gap = 4.0f)
{
	const float Length = FVector2f::Distance(A, B);
	if (Length <= 0.0f)
	{
		return;
	}
	const FVector2f Step = (B - A) / Length;
	for (float T = 0.0f; T < Length; T += Dash + Gap)
	{
		Lines(Out, Layer, Geometry, {A + Step * T, A + Step * FMath::Min(T + Dash, Length)}, Color, Thickness);
	}
}
} // namespace RigExecDraw
