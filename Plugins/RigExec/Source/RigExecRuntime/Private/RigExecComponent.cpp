#include "RigExecComponent.h"

#include "RigExecShaders.h"
#include "RigExecSkinComponent.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Components/DynamicMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "Dom/JsonObject.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UDynamicMesh.h"
#include "UnrealUSDWrapper.h"
#include "UsdWrappers/UsdStage.h"

// Unreal builds USD into the editor only; a packaged game loads no stage.
#if USE_USD_SDK
#include "USDTypesConversion.h"

#include "USDIncludesStart.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "USDIncludesEnd.h"
#endif

THIRD_PARTY_INCLUDES_START
#pragma push_macro("check")
#pragma push_macro("verify")
#undef check
#undef verify
#include "rigExecRuntime/runtime.h"
#pragma pop_macro("verify")
#pragma pop_macro("check")
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogRigExec, Log, All);

URigExecComponent::FOnRenderFallback URigExecComponent::OnRenderFallback;

using namespace UE::Geometry;

namespace
{
// A file property as a full path: a relative one is relative to the project
// directory, so a project carrying its rig data opens from any checkout.
FString
Resolve(const FFilePath& File)
{
	if (File.FilePath.IsEmpty() || !FPaths::IsRelative(File.FilePath))
	{
		return File.FilePath;
	}
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File.FilePath);
}

// The six rotation orders, in avars:rotationOrder's spelling.
const TCHAR* const GOrders[] = {TEXT("XYZ"), TEXT("XZY"), TEXT("YXZ"), TEXT("YZX"), TEXT("ZXY"), TEXT("ZYX")};

struct FRot3
{
	double M[3][3];
};

// A row-vector rotation about axis `Axis` by `Radians`, as Gf builds it.
FRot3
AxisRotation(int32 Axis, double Radians)
{
	const double C = FMath::Cos(Radians), S = FMath::Sin(Radians);
	FRot3 R = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	const int32 A = (Axis + 1) % 3, B = (Axis + 2) % 3;
	R.M[A][A] = C;
	R.M[A][B] = S;
	R.M[B][A] = -S;
	R.M[B][B] = C;
	return R;
}

FRot3
Multiply(const FRot3& L, const FRot3& R)
{
	FRot3 Out;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Out.M[Row][Col] = L.M[Row][0] * R.M[0][Col] + L.M[Row][1] * R.M[1][Col] + L.M[Row][2] * R.M[2][Col];
		}
	}
	return Out;
}

FRot3
Transpose(const FRot3& R)
{
	FRot3 Out;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Out.M[Row][Col] = R.M[Col][Row];
		}
	}
	return Out;
}

double
Unwrap(double Angle, double Hint)
{
	return Angle + 360.0 * FMath::RoundHalfFromZero((Hint - Angle) / 360.0);
}

// Euler angles in degrees whose rotations, applied in `Order` (row
// vectors, first axis first), reproduce `Rot`; of the two solutions the one
// nearest `Hint`, each angle unwrapped to the hint's turn. The derivation
// is gizmoMath.DecomposeEuler's.
void
DecomposeEuler(const FRot3& Rot, int32 Order, const double Hint[3], double Out[3])
{
	const TCHAR* Name = GOrders[Order];
	const int32 I = Name[0] - TEXT('X'), J = Name[1] - TEXT('X'), K = Name[2] - TEXT('X');
	const double Eps = (Order == 0 || Order == 3 || Order == 4) ? 1.0 : -1.0;
	auto M = [&Rot](int32 R, int32 C) { return Rot.M[C][R]; };

	const double CosBeta = FMath::Sqrt(M(K, J) * M(K, J) + M(K, K) * M(K, K));
	const double Beta = FMath::Atan2(-Eps * M(K, I), CosBeta);
	double Alpha, Gamma;
	if (CosBeta > 1e-9)
	{
		Alpha = FMath::Atan2(Eps * M(K, J), M(K, K));
		Gamma = FMath::Atan2(Eps * M(J, I), M(I, I));
	}
	else
	{
		// Gimbal lock: keep the hint's first angle, solve the last.
		Alpha = FMath::DegreesToRadians(Hint[I]);
		const FRot3 Residual = Multiply(Transpose(Multiply(AxisRotation(I, Alpha), AxisRotation(J, Beta))), Rot);
		Gamma = FMath::Atan2(Eps * Residual.M[I][J], Residual.M[I][I]);
	}
	double First[3], Second[3];
	First[I] = FMath::RadiansToDegrees(Alpha);
	First[J] = FMath::RadiansToDegrees(Beta);
	First[K] = FMath::RadiansToDegrees(Gamma);
	Second[I] = First[I] + 180.0;
	Second[J] = 180.0 - First[J];
	Second[K] = First[K] + 180.0;
	double Best = TNumericLimits<double>::Max();
	for (const double* Candidate : {First, Second})
	{
		double Unwrapped[3], Distance = 0.0;
		for (int32 A = 0; A < 3; ++A)
		{
			Unwrapped[A] = Unwrap(Candidate[A], Hint[A]);
			Distance += FMath::Abs(Unwrapped[A] - Hint[A]);
		}
		if (Distance < Best)
		{
			Best = Distance;
			FMemory::Memcpy(Out, Unwrapped, sizeof(Unwrapped));
		}
	}
}

#if USE_USD_SDK
// A material's look as a UsdPreviewSurface states it; anything else (a
// procedural shader) keeps the neutral default.
struct FLook
{
	FLinearColor Color = FLinearColor(0.7f, 0.7f, 0.7f);
	float Roughness = 0.6f;
	float Metallic = 0.0f;
};

FLook
ReadLook(const pxr::UsdShadeMaterial& Material)
{
	FLook Look;
	const pxr::UsdShadeShader Surface = Material ? Material.ComputeSurfaceSource() : pxr::UsdShadeShader();
	pxr::TfToken Id;
	if (!Surface || !Surface.GetShaderId(&Id) || Id != pxr::TfToken("UsdPreviewSurface"))
	{
		return Look;
	}
	pxr::GfVec3f Color;
	float Value = 0.0f;
	if (Surface.GetInput(pxr::TfToken("diffuseColor")).Get(&Color))
	{
		Look.Color = FLinearColor(Color[0], Color[1], Color[2]);
	}
	if (Surface.GetInput(pxr::TfToken("roughness")).Get(&Value))
	{
		Look.Roughness = Value;
	}
	if (Surface.GetInput(pxr::TfToken("metallic")).Get(&Value))
	{
		Look.Metallic = Value;
	}
	return Look;
}
#endif

const TCHAR* const GTransformAvars[9] = {TEXT(".avars:tx"), TEXT(".avars:ty"), TEXT(".avars:tz"),
                                         TEXT(".avars:rx"), TEXT(".avars:ry"), TEXT(".avars:rz"),
                                         TEXT(".avars:sx"), TEXT(".avars:sy"), TEXT(".avars:sz")};

// Touch highlight states, in drawing priority: a region hovered and
// selected draws as hovered. Their colours and opacity are usdview
// TouchPose's (plugin/touchPose/touchPoseModel.py, touchPoseUI.py).
enum ETouchState
{
	TouchHover,
	TouchLead,
	TouchSelected,
	TouchStateCount
};
// The tint's strength: usdview's HIGHLIGHT_OPACITY, carried in the colour's
// alpha, which the surface material blends by.
constexpr float GTouchOpacity = 0.30f;
// An untinted face: the material ignores its colour.
const FVector4f GNoTint(1.0f, 1.0f, 1.0f, 0.0f);
const FLinearColor GDefaultLeadColor(0.054f, 0.420f, 0.187f);
const FLinearColor GDefaultSelectedColor(0.277f, 0.277f, 0.277f);

// A colour scaled so its brightest channel is `Value`: hue and saturation
// stay exactly as authored (touchPoseModel._ScaleTo).
FLinearColor
ScaleTo(const FLinearColor& Color, float Value)
{
	const float Peak = FMath::Max3(Color.R, Color.G, Color.B);
	if (Peak <= 1e-6f)
	{
		return FLinearColor(Value, Value, Value);
	}
	const float Factor = Value / Peak;
	return FLinearColor(FMath::Min(1.0f, Color.R * Factor), FMath::Min(1.0f, Color.G * Factor),
	                    FMath::Min(1.0f, Color.B * Factor));
}
} // namespace

struct URigExecComponent::FRigExecTouch
{
	struct FMesh
	{
		/** The USD mesh's path, which the regions' scope names. */
		FString Path;
		/** The USD face each triangle came from. */
		TArray<int32> TriangleFace;
		/** Each USD face's first triangle; a face's triangles are contiguous
		 * (one past the last face holds the triangle count). */
		TArray<int32> FaceFirstTriangle;
		/** Each USD face's region, -1 for unpainted skin; empty when the
		 * mesh has no regions. */
		TArray<int32> FaceRegion;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
		/** The posed points moved since the tree was built. */
		bool bTreeDirty = true;
	};
	struct FRegion
	{
		int32 Mesh = -1;
		FString Name;
		FString ControlPath;
		FName Element;
		FLinearColor HoverColor;
		/** The USD faces it owns, on its mesh. */
		TArray<int32> Faces;
	};
	TArray<FMesh> Meshes;
	TArray<FRegion> Regions;
	FLinearColor LeadColor = ScaleTo(GDefaultLeadColor, 0.95f);
	FLinearColor SelectedColor = ScaleTo(GDefaultSelectedColor, 0.62f);
	int32 Hover = -1;
	int32 Lead = -1;
	TArray<int32> Selected;
	bool bSuspended = false;
	/** Each region's state as last written into the colours
	 * (TouchStateCount: untinted). */
	TArray<uint8> Written;

	bool IsShown() const { return Hover >= 0 || Lead >= 0 || Selected.Num() > 0; }
};

URigExecComponent::URigExecComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = true;
}
URigExecComponent::~URigExecComponent()
{
	delete Reader;
	delete Touch;
}

bool
URigExecComponent::Load()
{
	Bytes.Reset();
	bPlaced = false;
	if (!FFileHelper::LoadFileToArray(Bytes, *Resolve(RigExecFile)))
	{
		UE_LOG(LogRigExec, Error, TEXT("cannot read %s"), *RigExecFile.FilePath);
		return false;
	}
	std::string Error;
	delete Reader;
	Reader = rigExec::RigExecRuntimeReader::Open(Bytes.GetData(), size_t(Bytes.Num()), &Error).release();
	const std::vector<double> Frames = Reader ? Reader->GetFrameTimes() : std::vector<double>();
	if (!Reader || Frames.empty() || !Reader->SetFrame(Frames.front(), &Error) ||
	    !Reader->Execute(&Error))
	{
		UE_LOG(LogRigExec, Error, TEXT("cannot open the rig: %s"), UTF8_TO_TCHAR(Error.c_str()));
		delete Reader;
		Reader = nullptr;
		return false;
	}

#if USE_USD_SDK
	Stage = UnrealUSDWrapper::OpenStage(*Resolve(StageFile), EUsdInitialLoadSet::LoadAll);
	if (!Stage)
	{
		UE_LOG(LogRigExec, Error, TEXT("cannot open %s"), *StageFile.FilePath);
		return false;
	}
	const pxr::UsdStageRefPtr UsdStage(Stage);
	const FUsdStageInfo StageInfo(UsdStage);

	for (UDynamicMeshComponent* Mesh : Meshes)
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
	}
	for (URigExecSkinComponent* Skin : Skins)
	{
		if (Skin)
		{
			Skin->DestroyComponent();
		}
	}
	Skins.Reset();
	Meshes.Reset();
	PointSlots.Reset();
	MeshDrawn.Reset();
	delete Touch;
	Touch = new FRigExecTouch;
	const auto& Points = Reader->GetPoints();
	for (int32 Slot = 0; Slot < int32(Points.size()); ++Slot)
	{
		const std::string& Property = Points[Slot].path;
		const size_t Dot = Property.rfind('.');
		if (Dot == std::string::npos || Property.substr(Dot) != ".points")
		{
			continue;
		}
		const pxr::UsdGeomMesh UsdMesh(UsdStage->GetPrimAtPath(pxr::SdfPath(Property.substr(0, Dot))));
		if (!UsdMesh)
		{
			continue;
		}
		pxr::VtIntArray Counts, Indices;
		UsdMesh.GetFaceVertexCountsAttr().Get(&Counts);
		UsdMesh.GetFaceVertexIndicesAttr().Get(&Indices);
		const auto& Rig = Points[Slot].points;

		FDynamicMesh3 Built;
		for (const auto& P : Rig)
		{
			Built.AppendVertex(UsdToUnreal::ConvertVector(StageInfo, pxr::GfVec3f(P[0], P[1], P[2])));
		}
		// USD faces are counter-clockwise in a right-handed space. Swapping
		// Y and Z mirrors them into Unreal's left-handed space, where the
		// same index order is the front face, so the order is kept.
		// Material slots: the mesh's own binding, then one per bound face
		// subset, which overrides it for its faces.
		pxr::UsdShadeMaterialBindingAPI Binding(UsdMesh.GetPrim());
		TArray<pxr::UsdShadeMaterial> Slots;
		Slots.Add(Binding.ComputeBoundMaterial());
		TArray<int32> FaceSlot;
		FaceSlot.Init(0, int32(Counts.size()));
		for (const pxr::UsdGeomSubset& Subset : Binding.GetMaterialBindSubsets())
		{
			pxr::VtIntArray Faces;
			Subset.GetIndicesAttr().Get(&Faces);
			Slots.Add(pxr::UsdShadeMaterialBindingAPI(Subset.GetPrim()).ComputeBoundMaterial());
			for (int Face : Faces)
			{
				if (Face >= 0 && Face < FaceSlot.Num())
				{
					FaceSlot[Face] = Slots.Num() - 1;
				}
			}
		}
		TArray<int32> TriangleFace;
		TArray<FIndex3i> TriangleCorners;
		int32 Cursor = 0;
		for (int32 Face = 0; Face < int32(Counts.size()); ++Face)
		{
			const int Count = Counts[Face];
			for (int Corner = 1; Corner + 1 < Count; ++Corner)
			{
				const int A = Indices[Cursor];
				const int B = Indices[Cursor + Corner];
				const int C = Indices[Cursor + Corner + 1];
				if (A < int(Rig.size()) && B < int(Rig.size()) && C < int(Rig.size()))
				{
					if (Built.AppendTriangle(A, B, C) >= 0)
					{
						TriangleFace.Add(Face);
						TriangleCorners.Add(FIndex3i(Cursor, Cursor + Corner, Cursor + Corner + 1));
					}
				}
			}
			Cursor += Count;
		}
		Built.EnableAttributes();
		FMeshNormals::InitializeOverlayToPerVertexNormals(Built.Attributes()->PrimaryNormals(), false);
		Built.Attributes()->EnableMaterialID();
		for (int32 Triangle : Built.TriangleIndicesItr())
		{
			Built.Attributes()->GetMaterialID()->SetValue(Triangle, FaceSlot[TriangleFace[Triangle]]);
		}
		// Each USD face is a triangle group: a renderer that wants the real
		// polygons (a subdivision surface) gets the quads back from them.
		Built.EnableTriangleGroups();
		for (int32 Triangle : Built.TriangleIndicesItr())
		{
			Built.SetTriangleGroup(Triangle, TriangleFace[Triangle]);
		}
		// primvars:st, one element per face-vertex corner so faces keep their
		// own UVs across a seam; V flipped, as Unreal's UVs run top-down.
		{
			const pxr::UsdGeomPrimvar St = pxr::UsdGeomPrimvarsAPI(UsdMesh.GetPrim()).GetPrimvar(pxr::TfToken("st"));
			pxr::VtVec2fArray Values;
			if (St && St.ComputeFlattened(&Values) && !Values.empty())
			{
				const pxr::TfToken Interpolation = St.GetInterpolation();
				FDynamicMeshUVOverlay* UVs = Built.Attributes()->PrimaryUV();
				TArray<int32> Elements;
				Elements.SetNumUninitialized(int32(Indices.size()));
				int32 Corner = 0;
				for (int32 Face = 0; Face < int32(Counts.size()); ++Face)
				{
					for (int Side = 0; Side < Counts[Face]; ++Side, ++Corner)
					{
						size_t At = 0;
						if (Interpolation == pxr::UsdGeomTokens->faceVarying)
						{
							At = size_t(Corner);
						}
						else if (Interpolation == pxr::UsdGeomTokens->vertex || Interpolation == pxr::UsdGeomTokens->varying)
						{
							At = size_t(Indices[Corner]);
						}
						else if (Interpolation == pxr::UsdGeomTokens->uniform)
						{
							At = size_t(Face);
						}
						const pxr::GfVec2f UV = At < Values.size() ? Values[At] : pxr::GfVec2f(0.0f);
						Elements[Corner] = UVs->AppendElement(FVector2f(UV[0], 1.0f - UV[1]));
					}
				}
				for (int32 Triangle : Built.TriangleIndicesItr())
				{
					const FIndex3i& C = TriangleCorners[Triangle];
					UVs->SetTriangle(Triangle, FIndex3i(Elements[C.A], Elements[C.B], Elements[C.C]));
				}
			}
		}
		// A colour per face corner, untinted: Touch Pose lights a region by
		// writing its faces' colours, which the surface material blends by
		// their alpha.
		{
			Built.Attributes()->EnablePrimaryColors();
			FDynamicMeshColorOverlay* Colors = Built.Attributes()->PrimaryColors();
			for (int32 Triangle : Built.TriangleIndicesItr())
			{
				Colors->SetTriangle(Triangle, FIndex3i(Colors->AppendElement(GNoTint), Colors->AppendElement(GNoTint),
				                                       Colors->AppendElement(GNoTint)));
			}
		}

		// Rebuilt from the files on every load, so never saved.
		UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(
			GetOwner() ? static_cast<UObject*>(GetOwner()) : static_cast<UObject*>(this),
			*FString(UTF8_TO_TCHAR(UsdMesh.GetPrim().GetName().GetText())), RF_Transient);
		Component->SetMesh(MoveTemp(Built));
		// Deformed every evaluation, so never part of a lighting build.
		Component->SetMobility(EComponentMobility::Movable);
		UMaterialInterface* Parent = SurfaceMaterial ? SurfaceMaterial.Get()
		                                             : LoadObject<UMaterialInterface>(nullptr, TEXT("/RigExec/M_RigExecSurface.M_RigExecSurface"));
		if (!Parent)
		{
			Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		for (int32 Index = 0; Parent && Index < Slots.Num(); ++Index)
		{
			const FLook Look = ReadLook(Slots[Index]);
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Component);
			// The basic shape material calls its colour "Color"; a project
			// material may use the Unreal names.
			Instance->SetVectorParameterValue(TEXT("Color"), Look.Color);
			Instance->SetVectorParameterValue(TEXT("BaseColor"), Look.Color);
			Instance->SetScalarParameterValue(TEXT("Roughness"), Look.Roughness);
			Instance->SetScalarParameterValue(TEXT("Metallic"), Look.Metallic);
			Component->SetMaterial(Index, Instance);
		}
		if (GetOwner())
		{
			Component->SetupAttachment(this);
			Component->RegisterComponent();
		}
		Meshes.Add(Component);
		PointSlots.Add(Slot);
		MeshDrawn.Add(true);
		FRigExecTouch::FMesh& Touched = Touch->Meshes.AddDefaulted_GetRef();
		Touched.Path = UTF8_TO_TCHAR(UsdMesh.GetPrim().GetPath().GetString().c_str());
		Touched.FaceFirstTriangle.Init(TriangleFace.Num(), int32(Counts.size()) + 1);
		for (int32 Triangle = TriangleFace.Num() - 1; Triangle >= 0; --Triangle)
		{
			Touched.FaceFirstTriangle[TriangleFace[Triangle]] = Triangle;
		}
		// A face that lost every triangle starts where the next one does.
		for (int32 Face = int32(Counts.size()) - 1; Face >= 0; --Face)
		{
			Touched.FaceFirstTriangle[Face] = FMath::Min(Touched.FaceFirstTriangle[Face], Touched.FaceFirstTriangle[Face + 1]);
		}
		Touched.TriangleFace = MoveTemp(TriangleFace);
	}
	if (Meshes.Num() == 0)
	{
		UE_LOG(LogRigExec, Error, TEXT("no mesh in %s matches the rig's points"), *StageFile.FilePath);
	}
	CentimetresPerUnit = StageInfo.MetersPerUnit * 100.0;
	if (!ControlsFile.FilePath.IsEmpty())
	{
		LoadControls();
	}
	LoadTouchRegions();
	SetUpRenderPath();
	UE_LOG(LogRigExec, Log, TEXT("loaded %s: %d meshes, %d controls, %d channels, property chains %s"),
	       *RigExecFile.FilePath, Meshes.Num(), ControlMaps.Num(), ChannelMaps.Num(),
	       Reader->HasPropertyChains() ? TEXT("live") : TEXT("replayed"));
	return Meshes.Num() > 0;
#else
	UE_LOG(LogRigExec, Error, TEXT("this build has no USD; the meshes cannot be read"));
	return false;
#endif
}

bool
URigExecComponent::SetChannel(const FString& Channel, float Value)
{
	if (!Reader)
	{
		return false;
	}
	std::string Why;
	if (!Reader->SetAvar(TCHAR_TO_UTF8(*Channel), double(Value), &Why))
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s: %s"), *Channel, UTF8_TO_TCHAR(Why.c_str()));
		return false;
	}
	return true;
}

void
URigExecComponent::ClearChannels()
{
	if (Reader)
	{
		Reader->ClearAvars();
	}
	// Back to the described defaults, so the next pull resends what moved.
	for (FControlMap& Map : ControlMaps)
	{
		FMemory::Memcpy(Map.Values, Map.Defaults, sizeof(Map.Values));
	}
	for (FChannelMap& Map : ChannelMaps)
	{
		Map.Value = Map.Default;
	}
}

float
URigExecComponent::Evaluate()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Evaluate);
	if (!Reader)
	{
		return -1.0f;
	}
	std::string Error;
	const double Start = FPlatformTime::Seconds();
	bool bExecuted;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Execute);
		bExecuted = Reader->Execute(&Error);
	}
	if (!bExecuted)
	{
		UE_LOG(LogRigExec, Error, TEXT("evaluation failed: %s"), UTF8_TO_TCHAR(Error.c_str()));
		return -1.0f;
	}
#if USE_USD_SDK
	const auto& Points = Reader->GetPoints();
	const FUsdStageInfo StageInfo{pxr::UsdStageRefPtr(Stage)};
	for (int32 M = 0; M < Meshes.Num(); ++M)
	{
		const auto& Rig = Points[size_t(PointSlots[M])].points;
		// Three cases. The GPU skin draws it: the data mesh takes the points
		// quietly (picking and Touch Pose read them) and the skin uploads them.
		// Hidden, another renderer draws it (a subdivision surface, from these
		// points on the GPU): positions only, with the change events it
		// listens for. Otherwise the CPU fallback: positions and normals.
		TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_UpdateMesh);
		const bool bSkin = SkinDraws(M);
		const bool bDrawn = !bGpu && Meshes[M]->IsVisible();
		MeshDrawn[M] = bDrawn;
		Meshes[M]->GetDynamicMesh()->EditMesh(
			[&](FDynamicMesh3& Mesh)
			{
				const int32 Count = FMath::Min(Mesh.MaxVertexID(), int32(Rig.size()));
				for (int32 V = 0; V < Count; ++V)
				{
					const auto& P = Rig[size_t(V)];
					Mesh.SetVertex(V, UsdToUnreal::ConvertVector(StageInfo, pxr::GfVec3f(P[0], P[1], P[2])));
				}
				// Normals from the deformed points every evaluation -- skin,
				// blend shapes and every other deformer included, as a
				// skeletal mesh's recompute-normals option does for morphs.
				if (bDrawn)
				{
					FMeshNormals::QuickRecomputeOverlayNormals(Mesh);
				}
			},
			// Positions AND normals changed, or the renderer keeps the rest
			// normals and the shading lags the shape.
			EDynamicMeshChangeType::DeformationEdit,
			bDrawn ? EDynamicMeshAttributeChangeFlags::VertexPositions | EDynamicMeshAttributeChangeFlags::NormalsTangents
			       : EDynamicMeshAttributeChangeFlags::VertexPositions,
			/*bDeferChangeEvents*/ bSkin);
		if (bSkin)
		{
			Skins[M]->UpdatePositions(*Meshes[M]->GetMesh());
		}
	}
	LastPoseTime = FPlatformTime::Seconds();
	// The skin moved: picks need a new tree. The highlight is in the
	// meshes' colours and moves with them.
	if (Touch)
	{
		for (FRigExecTouch::FMesh& Touched : Touch->Meshes)
		{
			Touched.bTreeDirty = true;
		}
	}
#endif
	return float((FPlatformTime::Seconds() - Start) * 1000.0);
}

bool
URigExecComponent::SkinDraws(int32 M) const
{
	return bGpu && Skins.IsValidIndex(M) && Skins[M] && Meshes.IsValidIndex(M) && Meshes[M] && Meshes[M]->IsVisible();
}

void
URigExecComponent::SetUpRenderPath()
{
	bGpu = false;
	FallbackReason.Reset();
	if (!bDrawOnGPU)
	{
		FallbackReason = TEXT("Draw On GPU is off for this component");
	}
	else if (RigExecShaders::CanUseGpuSkin(&FallbackReason))
	{
		bGpu = GetOwner() != nullptr;
		if (!bGpu)
		{
			FallbackReason = TEXT("the component has no actor to put its GPU meshes on");
		}
	}
	int32 RenderVertices = 0;
	for (int32 M = 0; bGpu && M < Meshes.Num(); ++M)
	{
		UDynamicMeshComponent* Data = Meshes[M];
		URigExecSkinComponent* Skin = NewObject<URigExecSkinComponent>(GetOwner(), *(Data->GetName() + TEXT("_GPU")), RF_Transient);
		Skin->SetMobility(EComponentMobility::Movable);
		Skin->bMotionBlur = bMotionBlur;
		Skin->SetupAttachment(Data);
		TArray<UMaterialInterface*> Materials;
		for (int32 Slot = 0; Slot < Data->GetNumMaterials(); ++Slot)
		{
			Materials.Add(Data->GetMaterial(Slot));
		}
		Skin->Build(*Data->GetMesh(), Materials);
		Skin->RegisterComponent();
		Skin->SetVisibility(Data->IsVisible());
		RenderVertices += Skin->GetRenderVertexCount();
		Skins.Add(Skin);
		// The data mesh stays -- picking, Touch Pose and a subdivision surface
		// read it, and its visibility says whether anything else draws it --
		// but no longer draws itself.
		Data->SetRenderInMainPass(false);
		Data->SetRenderInDepthPass(false);
		Data->SetCastShadow(false);
		Data->SetVisibleInRayTracing(false);
	}
	if (bGpu)
	{
		RenderPath = FString::Printf(TEXT("GPU: %d meshes, %d vertices; each pose uploads the points once and the GPU computes normals%s"),
		                             Skins.Num(), RenderVertices, bMotionBlur ? TEXT(", with motion vectors") : TEXT(""));
		UE_LOG(LogRigExec, Log, TEXT("%s draws on the GPU (%d meshes, %d render vertices)"), *GetPathName(), Skins.Num(), RenderVertices);
		return;
	}
	RenderPath = TEXT("CPU fallback: ") + FallbackReason;
	if (bDrawOnGPU)
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s draws on the CPU: %s"), *GetPathName(), *FallbackReason);
		OnRenderFallback.Broadcast(this, FallbackReason);
	}
	else
	{
		UE_LOG(LogRigExec, Log, TEXT("%s draws on the CPU: %s"), *GetPathName(), *FallbackReason);
	}
}

#if WITH_EDITOR
void
URigExecComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// The render path is chosen when the meshes are built: rebuild them.
	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	if ((Changed == GET_MEMBER_NAME_CHECKED(URigExecComponent, bDrawOnGPU) ||
	     Changed == GET_MEMBER_NAME_CHECKED(URigExecComponent, bMotionBlur)) && Reader)
	{
		Load();
	}
}
#endif

FVector
URigExecComponent::GetVertex(int32 Mesh, int32 Index) const
{
	if (!Meshes.IsValidIndex(Mesh) || !Meshes[Mesh])
	{
		return FVector::ZeroVector;
	}
	const FDynamicMesh3* Built = Meshes[Mesh]->GetMesh();
	return Built && Built->IsVertex(Index) ? FVector(Built->GetVertex(Index)) : FVector::ZeroVector;
}

bool
URigExecComponent::LoadControls()
{
	ControlMaps.Reset();
	ChannelMaps.Reset();
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *Resolve(ControlsFile)) ||
	    !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root)
	{
		UE_LOG(LogRigExec, Error, TEXT("cannot read %s"), *ControlsFile.FilePath);
		return false;
	}
	// The Control Rig is in USD space (build_control_rig.py): every control
	// keeps its USD offset and value as authored, and one root null converts
	// the whole asset into Unreal's space.
	FString Space, RootName;
	if (!Root->TryGetStringField(TEXT("space"), Space) || Space != TEXT("usd"))
	{
		UE_LOG(LogRigExec, Error,
		       TEXT("%s is not a USD-space controls file; rebuild the Control Rig with build_control_rig.py"),
		       *ControlsFile.FilePath);
		return false;
	}
	UsdRoot = FName(Root->TryGetStringField(TEXT("root"), RootName) ? RootName : FString(TEXT("UsdSpace")));
	const TArray<TSharedPtr<FJsonValue>>* Placement = nullptr;
	if (Root->TryGetArrayField(TEXT("assetToWorld"), Placement) && Placement->Num() == 16)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			AssetToWorld[I] = (*Placement)[I]->AsNumber();
		}
	}
	TArray<FString> Paths;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("controls")))
	{
		const TSharedPtr<FJsonObject>& Control = Value->AsObject();
		FControlMap& Map = ControlMaps.AddDefaulted_GetRef();
		Map.Element = FName(Control->GetStringField(TEXT("element")));
		Map.Path = Control->GetStringField(TEXT("path"));
		Map.Parent = int32(Control->GetNumberField(TEXT("parent")));
		FString Kind;
		Map.bNull = Control->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("null");
		Paths.Add(Map.Path);
		const FString Order = Control->GetStringField(TEXT("rotationOrder"));
		for (int32 O = 0; O < 6; ++O)
		{
			if (Order == GOrders[O])
			{
				Map.Order = O;
			}
		}
		const auto& Sign = Control->GetArrayField(TEXT("rotationSign"));
		const auto& Defaults = Control->GetArrayField(TEXT("defaults"));
		const TCHAR* const Groups[3] = {TEXT("translate"), TEXT("rotate"), TEXT("scale")};
		for (int32 A = 0; A < 9; ++A)
		{
			Map.Channels[A] = Control->GetArrayField(Groups[A / 3])[A % 3]->AsBool();
			Map.Values[A] = Map.Defaults[A] = Defaults[A]->AsNumber();
		}
		for (int32 A = 0; A < 3; ++A)
		{
			Map.Sign[A] = Sign[A]->AsNumber();
		}
		Map.UnitScale = Control->GetNumberField(TEXT("unitScale"));
	}
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("channels")))
	{
		const TSharedPtr<FJsonObject>& Channel = Value->AsObject();
		FChannelMap& Map = ChannelMaps.AddDefaulted_GetRef();
		Map.Element = FName(Channel->GetStringField(TEXT("element")));
		Map.Path = Paths[int32(Channel->GetNumberField(TEXT("control")))] + TEXT(".") +
		           Channel->GetStringField(TEXT("attr"));
		Map.Value = Map.Default = Channel->GetNumberField(TEXT("default"));
		const TArray<TSharedPtr<FJsonValue>>* Labels = nullptr;
		if (Channel->TryGetArrayField(TEXT("labels"), Labels))
		{
			for (const TSharedPtr<FJsonValue>& Label : *Labels)
			{
				Map.Labels.Add(Label->AsString());
			}
		}
	}
	// The binary runtime takes transform avars and property chain inputs
	// only; a channel it refuses (a mode switch read at bake time) cannot
	// pose it and is dropped.
	if (Reader)
	{
		const int32 Described = ChannelMaps.Num();
		TArray<FString> Dropped;
		ChannelMaps.RemoveAll(
			[this, &Dropped](const FChannelMap& Map)
			{
				if (Reader->SetAvar(TCHAR_TO_UTF8(*Map.Path), Map.Value, nullptr))
				{
					return false;
				}
				Dropped.Add(FPaths::GetExtension(Map.Path.Replace(TEXT(":"), TEXT("_"))) + TEXT(" on ") +
				            FPaths::GetBaseFilename(Map.Path.Left(Map.Path.Find(TEXT("."), ESearchCase::CaseSensitive,
				                                                            ESearchDir::FromEnd))));
				return true;
			});
		Reader->ClearAvars();
		if (Dropped.Num() > 0)
		{
			UE_LOG(LogRigExec, Log, TEXT("%d of %d channels are not live in %s: %s"), Dropped.Num(), Described,
			       *RigExecFile.FilePath, *FString::Join(Dropped, TEXT(", ")));
		}
	}
	// Each control's fade, once the channels the runtime takes are known.
	{
		const TArray<TSharedPtr<FJsonValue>>& Controls = Root->GetArrayField(TEXT("controls"));
		for (int32 C = 0; C < Controls.Num() && C < ControlMaps.Num(); ++C)
		{
			const TSharedPtr<FJsonObject>* Fade = nullptr;
			if (!Controls[C]->AsObject()->TryGetObjectField(TEXT("fade"), Fade))
			{
				continue;
			}
			const FString Source = (*Fade)->GetStringField(TEXT("source"));
			ControlMaps[C].bFadeInvert = (*Fade)->GetBoolField(TEXT("invert"));
			for (int32 K = 0; K < ChannelMaps.Num(); ++K)
			{
				if (ChannelMaps[K].Path == Source)
				{
					ControlMaps[C].FadeChannel = K;
				}
			}
		}
	}
	// The IK/FK limbs and their rest offsets, for matching.
	LimbMaps.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Limbs = nullptr;
	if (Root->TryGetArrayField(TEXT("limbs"), Limbs))
	{
		auto Strings = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) {
			TArray<FString> Out;
			for (const TSharedPtr<FJsonValue>& V : Object->GetArrayField(Field))
			{
				Out.Add(V->AsString());
			}
			return Out;
		};
		auto Matrix = [](const TArray<TSharedPtr<FJsonValue>>& V) {
			FMatrix M = FMatrix::Identity;
			for (int32 I = 0; I < 16 && I < V.Num(); ++I)
			{
				M.M[I / 4][I % 4] = V[I]->AsNumber();
			}
			return M;
		};
		for (const TSharedPtr<FJsonValue>& Value : *Limbs)
		{
			const TSharedPtr<FJsonObject> L = Value->AsObject();
			FLimbMap Limb;
			Limb.Switch = L->GetStringField(TEXT("switch"));
			Limb.IkValue = L->GetNumberField(TEXT("ikValue"));
			Limb.Joints = Strings(L, TEXT("joints"));
			Limb.FkControls = Strings(L, TEXT("fkControls"));
			Limb.IkControl = L->GetStringField(TEXT("ikControl"));
			Limb.Effector = L->GetStringField(TEXT("effector"));
			Limb.Pole = L->GetStringField(TEXT("pole"));
			for (const TSharedPtr<FJsonValue>& Offset : L->GetArrayField(TEXT("fkOffsets")))
			{
				Limb.FkOffsets.Add(Matrix(Offset->AsArray()));
			}
			Limb.EffectorOffset = Matrix(L->GetArrayField(TEXT("effectorOffset")));
			Limb.PoleDistance = L->GetNumberField(TEXT("poleDistance"));
			if (Limb.Joints.Num() == 3 && Limb.FkControls.Num() == 3 && Limb.FkOffsets.Num() == 3)
			{
				LimbMaps.Add(MoveTemp(Limb));
			}
		}
	}
	return true;
}

int32
URigExecComponent::PullControls()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_PullControls);
	if (!Reader)
	{
		return 0;
	}
	if (!ControlRig && GetOwner())
	{
		ControlRig = GetOwner()->FindComponentByClass<UControlRigComponent>();
	}
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
	if (!Hierarchy)
	{
		return 0;
	}
	// Change test, in avar units: degrees and rig distance. A resync sends
	// everything whatever it held.
	constexpr double Tolerance = 1e-5;
	const bool bForce = bResync;
	bResync = false;
	if (bForce)
	{
		bPlaced = false;
		if (Touch)
		{
			// Unknown, so every region's colour is written again.
			Touch->Written.Init(uint8(TouchStateCount + 1), Touch->Regions.Num());
		}
	}
	int32 Changed = 0;
	auto Send = [&](const FString& Path, double& Held, double Value)
	{
		if (!bForce && FMath::Abs(Value - Held) <= Tolerance)
		{
			return;
		}
		std::string Why;
		if (Reader->SetAvar(TCHAR_TO_UTF8(*Path), Value, &Why))
		{
			Held = Value;
			++Changed;
		}
	};
	for (FControlMap& Map : ControlMaps)
	{
		const FRigElementKey Key(Map.Element, ERigElementType::Control);
		if (Map.bNull || !Hierarchy->Contains(Key))
		{
			continue;
		}
		// The control's value IS the avars matrix (USD space): read as a local.
		const FMatrix Ue = Hierarchy->GetLocalTransform(Key).ToMatrixWithScale();
		FRot3 Rot;
		double Scale[3], Translate[3];
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				Rot.M[Row][Col] = Ue.M[Row][Col];
			}
			Scale[Row] = FMath::Sqrt(Rot.M[Row][0] * Rot.M[Row][0] + Rot.M[Row][1] * Rot.M[Row][1] +
			                         Rot.M[Row][2] * Rot.M[Row][2]);
			for (int32 Col = 0; Col < 3; ++Col)
			{
				Rot.M[Row][Col] /= FMath::Max(Scale[Row], 1e-12);
			}
			Translate[Row] = Ue.M[3][Row] / CentimetresPerUnit;
		}
		// The sign multiplies the raw avar, so the hint is the effective
		// angle and the raw value is effective * sign.
		double Hint[3], Euler[3];
		for (int32 A = 0; A < 3; ++A)
		{
			Hint[A] = Map.Values[3 + A] * Map.Sign[A];
		}
		DecomposeEuler(Rot, Map.Order, Hint, Euler);
		for (int32 A = 0; A < 3; ++A)
		{
			if (Map.Channels[A])
			{
				Send(Map.Path + GTransformAvars[A], Map.Values[A], Translate[A] / Map.UnitScale);
			}
			if (Map.Channels[3 + A])
			{
				Send(Map.Path + GTransformAvars[3 + A], Map.Values[3 + A], Euler[A] * Map.Sign[A]);
			}
			if (Map.Channels[6 + A])
			{
				Send(Map.Path + GTransformAvars[6 + A], Map.Values[6 + A], Scale[A]);
			}
		}
	}
	for (FChannelMap& Map : ChannelMaps)
	{
		const FRigElementKey Key(Map.Element, ERigElementType::Control);
		if (Hierarchy->Contains(Key))
		{
			Send(Map.Path, Map.Value, double(Hierarchy->GetControlValue(Key).Get<float>()));
		}
	}
	// A limb's IK controls show only while it is in IK and its FK ones
	// only in FK, as the viewport fade does in usdview: shown while the
	// fade is above zero, so a hand-over keeps both halves grabbable.
	for (FControlMap& Map : ControlMaps)
	{
		if (Map.FadeChannel < 0 || Map.bNull || !ChannelMaps.IsValidIndex(Map.FadeChannel))
		{
			continue;
		}
		const double Weight = ChannelMaps[Map.FadeChannel].Value;
		const double Opacity = Map.bFadeInvert ? 1.0 - Weight : Weight;
		const int8 Shown = Opacity > 1e-3 ? 1 : 0;
		if (Shown != Map.Shown)
		{
			Map.Shown = Shown;
			Hierarchy->SetControlVisibility(FRigElementKey(Map.Element, ERigElementType::Control), Shown != 0);
		}
	}
	if (Changed > 0 || bForce)
	{
		Evaluate();
	}
	if (bForce)
	{
		RefreshTouchColors();
	}
	if (Changed > 0 || !bPlaced)
	{
		PlaceControls();
		bPlaced = true;
	}
	return Changed;
}

void
URigExecComponent::PlaceControls()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_PlaceControls);
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
	if (!Reader || !Hierarchy)
	{
		return;
	}
	// Each control's posed frame in Unreal's space, or none when the rig has
	// no slot for it (the gizmo then keeps its rest offset). The USD-space
	// root is read once, not once per control.
	const FMatrix RootGlobal = UsdRootGlobal();
	TArray<TOptional<FMatrix>> Frames;
	Frames.SetNum(ControlMaps.Num());
	for (int32 C = 0; C < ControlMaps.Num(); ++C)
	{
		FMatrix Asset;
		if (AssetFrame(ControlMaps[C].Path, Asset))
		{
			Frames[C] = ToUnreal(Asset, RootGlobal);
		}
	}
	// A parent's global once placed is its frame (parents are placed first):
	// take it from the frames rather than the hierarchy, which would
	// recompute it -- and its chain -- right after it was moved.
	auto ParentFrame = [&](int32 C) -> FMatrix
	{
		const int32 Parent = ControlMaps[C].Parent;
		if (Parent >= 0 && Frames[Parent].IsSet())
		{
			return Frames[Parent].GetValue();
		}
		return Parent >= 0 ? ParentGlobal(Hierarchy, C) : RootGlobal;
	};
	auto KeyOf = [this](int32 C)
	{
		return FRigElementKey(ControlMaps[C].Element,
		                      ControlMaps[C].bNull ? ERigElementType::Null : ERigElementType::Control);
	};
	for (int32 C = 0; C < ControlMaps.Num(); ++C)
	{
		const FControlMap& Map = ControlMaps[C];
		const FRigElementKey Key = KeyOf(C);
		if (!Frames[C].IsSet() || !Hierarchy->Contains(Key))
		{
			continue;
		}
		// Under the USD-space root every global is mirrored (the Y/Z swap);
		// locals are not. Everything is set as a local, so no FTransform ever
		// carries the mirror: a null's local is its frame in its parent's, a
		// control's offset its frame without its value.
		const FMatrix ParentInverse = ParentFrame(C).Inverse();
		if (Map.bNull)
		{
			Hierarchy->SetLocalTransform(Key, FTransform(Frames[C].GetValue() * ParentInverse), false, true);
			continue;
		}
		const FMatrix Value = Hierarchy->GetLocalTransform(Key).ToMatrixWithScale();
		Hierarchy->SetControlOffsetTransform(Key, FTransform(Value.Inverse() * Frames[C].GetValue() * ParentInverse),
		                                     false, true);
	}
}

void
URigExecComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                 FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// The GPU skins draw while their mesh is shown, and hide when another
	// renderer (a subdivision surface) hides it; shown again, a skin catches
	// up with the pose and colours it missed.
	for (int32 M = 0; bGpu && M < Skins.Num() && M < Meshes.Num(); ++M)
	{
		URigExecSkinComponent* Skin = Skins[M];
		const bool bShown = Meshes[M] && Meshes[M]->IsVisible();
		if (Skin && Skin->IsVisible() != bShown)
		{
			Skin->SetVisibility(bShown);
			if (bShown)
			{
				Skin->UpdatePositions(*Meshes[M]->GetMesh());
				Skin->UpdateColors(*Meshes[M]->GetMesh());
			}
		}
	}
	// A mesh another renderer drew is shown again: its normals were not kept.
	for (int32 M = 0; !bGpu && M < Meshes.Num() && M < MeshDrawn.Num(); ++M)
	{
		if (!MeshDrawn[M] && Meshes[M] && Meshes[M]->IsVisible())
		{
			MeshDrawn[M] = true;
			Meshes[M]->GetDynamicMesh()->EditMesh(
				[](FDynamicMesh3& Mesh) { FMeshNormals::QuickRecomputeOverlayNormals(Mesh); },
				EDynamicMeshChangeType::DeformationEdit,
				EDynamicMeshAttributeChangeFlags::VertexPositions | EDynamicMeshAttributeChangeFlags::NormalsTangents);
		}
	}
	PullControls();
}

void
URigExecComponent::OnRegister()
{
	Super::OnRegister();
#if WITH_EDITOR
	// An undo or redo can restore control values, meshes or colours behind
	// this component's back: resync after each one.
	if (!IsTemplate() && !UndoRedoHandle.IsValid())
	{
		UndoRedoHandle = FEditorDelegates::PostUndoRedo.AddUObject(this, &URigExecComponent::RequestResync);
	}
#endif
	if (!ControlRig && GetOwner())
	{
		ControlRig = GetOwner()->FindComponentByClass<UControlRigComponent>();
	}
	if (ControlRig)
	{
		PrimaryComponentTick.AddPrerequisite(ControlRig, ControlRig->PrimaryComponentTick);
	}
	// A saved level keeps the file paths, not the meshes: rebuild them.
	if (!IsTemplate() && !Reader && !RigExecFile.FilePath.IsEmpty() && !StageFile.FilePath.IsEmpty() &&
	    GetWorld())
	{
		Load();
	}
}

void
URigExecComponent::RequestResync()
{
	// Now, for what the undo itself restored; and on the next tick, for what
	// Sequencer restores when it re-evaluates afterwards.
	bResync = true;
	PullControls();
	bResync = true;
}

void
URigExecComponent::OnUnregister()
{
#if WITH_EDITOR
	FEditorDelegates::PostUndoRedo.Remove(UndoRedoHandle);
	UndoRedoHandle.Reset();
#endif
	Super::OnUnregister();
}

FName
URigExecComponent::FindControlElement(const FString& Path) const
{
	for (const FControlMap& Map : ControlMaps)
	{
		if (Map.Path == Path)
		{
			return Map.Element;
		}
	}
	return NAME_None;
}

FName
URigExecComponent::FindChannelElement(const FString& AttrPath) const
{
	for (const FChannelMap& Map : ChannelMaps)
	{
		if (Map.Path == AttrPath)
		{
			return Map.Element;
		}
	}
	return NAME_None;
}

FString
URigExecComponent::PickerFilePath() const
{
	FString Controls = Resolve(ControlsFile);
	if (Controls.IsEmpty())
	{
		return FString();
	}
	// Biped_controls.json -> Biped_picker.json (anything after "_controls"
	// is ignored).
	FString Picker = Controls;
	const int32 At = Controls.Find(TEXT("_controls"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (At != INDEX_NONE && Controls.EndsWith(TEXT(".json")))
	{
		Picker = Controls.Left(At) + TEXT("_picker.json");
	}
	return Picker != Controls && FPaths::FileExists(Picker) ? Picker : FString();
}

bool
URigExecComponent::AssetFrame(const FString& Path, FMatrix& Out) const
{
	double F[16];
	if (!Reader || !Reader->GetControlFrame(TCHAR_TO_UTF8(*Path), F))
	{
		return false;
	}
	for (int32 I = 0; I < 16; ++I)
	{
		Out.M[I / 4][I % 4] = F[I];
	}
	return true;
}

FMatrix
URigExecComponent::ToUnreal(const FMatrix& Asset) const
{
	return ToUnreal(Asset, UsdRootGlobal());
}

FMatrix
URigExecComponent::ToUnreal(const FMatrix& Asset, const FMatrix& RootGlobal) const
{
	double W[16];
	for (int32 R = 0; R < 4; ++R)
	{
		for (int32 K = 0; K < 4; ++K)
		{
			W[R * 4 + K] = Asset.M[R][0] * AssetToWorld[0 * 4 + K] + Asset.M[R][1] * AssetToWorld[1 * 4 + K] +
			               Asset.M[R][2] * AssetToWorld[2 * 4 + K] + Asset.M[R][3] * AssetToWorld[3 * 4 + K];
		}
	}
	// USD's own frame, in centimetres, then the root's conversion: the frame
	// the Control Rig shows under its USD-space root.
	FMatrix M = FMatrix::Identity;
	for (int32 R = 0; R < 3; ++R)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			M.M[R][K] = W[R * 4 + K];
		}
		M.M[3][R] = W[12 + R] * CentimetresPerUnit;
	}
	return M * RootGlobal;
}

FMatrix
URigExecComponent::UsdRootGlobal() const
{
	const UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	const URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
	const FRigElementKey Key(UsdRoot, ERigElementType::Null);
	if (Hierarchy && Hierarchy->Contains(Key))
	{
		return Hierarchy->GetGlobalTransform(Key).ToMatrixWithScale();
	}
	FMatrix Swapped = FMatrix::Identity;
	Swapped.M[1][1] = Swapped.M[2][2] = 0.0;
	Swapped.M[1][2] = Swapped.M[2][1] = 1.0;
	return Swapped;
}

FMatrix
URigExecComponent::ParentGlobal(const URigHierarchy* Hierarchy, int32 C) const
{
	const int32 Parent = ControlMaps[C].Parent;
	if (Parent >= 0)
	{
		const FRigElementKey Key(ControlMaps[Parent].Element,
		                         ControlMaps[Parent].bNull ? ERigElementType::Null : ERigElementType::Control);
		return Hierarchy->GetGlobalTransform(Key).ToMatrixWithScale();
	}
	return UsdRootGlobal();
}

bool
URigExecComponent::PutControl(const FString& Path, const FMatrix& Asset, bool bTranslateOnly)
{
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	const FName Element = FindControlElement(Path);
	if (!Rig || Element.IsNone())
	{
		return false;
	}
	// The control's value is its frame without its offset; the mirrored
	// global never becomes an FTransform.
	URigHierarchy* Hierarchy = Rig->GetHierarchy();
	const FRigElementKey Key(Element, ERigElementType::Control);
	const FMatrix Local = ToUnreal(Asset) * Hierarchy->GetGlobalControlOffsetTransform(Key).ToMatrixWithScale().Inverse();
	FTransform Value(Local);
	if (bTranslateOnly)
	{
		Value = Hierarchy->GetLocalTransform(Key);
		Value.SetTranslation(Local.GetOrigin());
	}
	Rig->SetControlLocalTransform(Element, Value, true, FRigControlModifiedContext(EControlRigSetKey::DoNotCare), true);
	PullControls();
	return true;
}

FString
URigExecComponent::FindLimbSwitchFor(FName Element) const
{
	const FControlMap* Control =
		ControlMaps.FindByPredicate([Element](const FControlMap& M) { return !M.bNull && M.Element == Element; });
	if (!Control)
	{
		return FString();
	}
	const FString& Path = Control->Path;
	for (const FLimbMap& Limb : LimbMaps)
	{
		FString SwitchControl;
		Limb.Switch.Split(TEXT("."), &SwitchControl, nullptr);
		if (Path == SwitchControl || Limb.FkControls.Contains(Path) || Path == Limb.IkControl ||
		    (!Limb.Pole.IsEmpty() && Path == Limb.Pole) || Path.StartsWith(Limb.IkControl + TEXT("/")))
		{
			return Limb.Switch;
		}
	}
	return FString();
}

bool
URigExecComponent::IsLimbIk(const FString& SwitchPath) const
{
	const FLimbMap* Limb = LimbMaps.FindByPredicate([&](const FLimbMap& L) { return L.Switch == SwitchPath; });
	const UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	const FName SwitchElement = FindChannelElement(SwitchPath);
	if (!Limb || !Rig || SwitchElement.IsNone())
	{
		return false;
	}
	const double Now = Rig->GetHierarchy()->GetControlValue(FRigElementKey(SwitchElement, ERigElementType::Control)).Get<float>();
	return FMath::Abs(Now - Limb->IkValue) < FMath::Abs(Now - (1.0 - Limb->IkValue));
}

bool
URigExecComponent::FindSpaceChannel(FName Element, FName& OutChannel, TArray<FString>& OutLabels) const
{
	const FControlMap* Control =
		ControlMaps.FindByPredicate([Element](const FControlMap& M) { return !M.bNull && M.Element == Element; });
	if (!Control)
	{
		return false;
	}
	const FString Path = Control->Path + TEXT(".avars:space");
	const FChannelMap* Space = ChannelMaps.FindByPredicate([&Path](const FChannelMap& M) { return M.Path == Path; });
	if (!Space || Space->Labels.Num() < 2)
	{
		return false;
	}
	OutChannel = Space->Element;
	OutLabels = Space->Labels;
	return true;
}

bool
URigExecComponent::SwitchSpace(FName Element, int32 Index)
{
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	FName Channel;
	TArray<FString> Labels;
	if (!Rig || !FindSpaceChannel(Element, Channel, Labels) || !Labels.IsValidIndex(Index))
	{
		return false;
	}
	// Keep the control where it was: evaluate in the new space (which
	// re-places the controls), put the control back on its old frame, and
	// evaluate again for whatever it carries -- the picker's space switch.
	const FRigControlModifiedContext Context(EControlRigSetKey::DoNotCare);
	const FTransform Before = Rig->GetControlGlobalTransform(Element);
	Rig->SetControlValue<float>(Channel, float(Index), true, Context, true);
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		PullControls();
		Rig->SetControlGlobalTransform(Element, Before, true, Context, true);
	}
	PullControls();
	return true;
}

bool
URigExecComponent::IsLimbSwitch(const FString& SwitchPath) const
{
	return LimbMaps.ContainsByPredicate([&](const FLimbMap& L) { return L.Switch == SwitchPath; });
}

bool
URigExecComponent::SwitchLimb(const FString& SwitchPath)
{
	const FLimbMap* Limb = LimbMaps.FindByPredicate([&](const FLimbMap& L) { return L.Switch == SwitchPath; });
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	const FName SwitchElement = FindChannelElement(SwitchPath);
	if (!Limb || !Rig || SwitchElement.IsNone())
	{
		return false;
	}
	// Start from what the rig shows now.
	PullControls();
	const double FkValue = 1.0 - Limb->IkValue;
	const double Now = Rig->GetControlValue(SwitchElement).Get<float>();
	const bool bToIk = FMath::Abs(Now - Limb->IkValue) >= FMath::Abs(Now - FkValue);
	FMatrix Joints[3];
	for (int32 J = 0; J < 3; ++J)
	{
		if (!AssetFrame(Limb->Joints[J], Joints[J]))
		{
			return false;
		}
	}
	// Row vectors throughout, as ikfkMatch: a child's frame is local *
	// parent. The inactive half moves nothing while it is inactive, so the
	// joints read above stay the target while its controls are placed.
	if (bToIk)
	{
		FMatrix Control, Effector;
		if (!AssetFrame(Limb->IkControl, Control) || !AssetFrame(Limb->Effector, Effector))
		{
			return false;
		}
		// The effector rides the IK control (through a foot's roll stack,
		// whose current values are kept): carry that relationship as is.
		const FMatrix Carry = Effector * Control.Inverse();
		const FMatrix Target = Carry.Inverse() * (Limb->EffectorOffset.Inverse() * Joints[2]);
		PutControl(Limb->IkControl, Target, false);
		FMatrix Pole;
		if (!Limb->Pole.IsEmpty() && Limb->PoleDistance > 0.0 && AssetFrame(Limb->Pole, Pole))
		{
			// On the bend plane, on the side the middle joint bends toward;
			// a limb too straight for a plane keeps the pole's direction.
			const FVector Root = Joints[0].GetOrigin();
			const FVector Middle = Joints[1].GetOrigin();
			const FVector Line = Joints[2].GetOrigin() - Root;
			FVector Where = Pole.GetOrigin();
			if (Line.Size() > 1e-9)
			{
				const FVector Axis = Line.GetUnsafeNormal();
				FVector Bend = (Middle - Root) - Axis * FVector::DotProduct(Middle - Root, Axis);
				if (Bend.Size() <= 1e-4 * Line.Size())
				{
					Bend = Where - Middle;
					Bend -= Axis * FVector::DotProduct(Bend, Axis);
				}
				if (Bend.Size() > 1e-9)
				{
					Where = Middle + Bend.GetUnsafeNormal() * Limb->PoleDistance;
				}
			}
			FMatrix PoleTarget = Pole;
			PoleTarget.SetOrigin(Where);
			PutControl(Limb->Pole, PoleTarget, true);
		}
	}
	else
	{
		// Root to end, each control evaluated before the next is placed.
		for (int32 J = 0; J < 3; ++J)
		{
			PutControl(Limb->FkControls[J], Limb->FkOffsets[J] * Joints[J], false);
		}
	}
	Rig->SetControlValue<float>(SwitchElement, float(bToIk ? Limb->IkValue : FkValue), true,
	                            FRigControlModifiedContext(EControlRigSetKey::DoNotCare), true);
	PullControls();
	return true;
}

FTransform
URigExecComponent::GetRigFrame(const FString& Path) const
{
	FMatrix Asset;
	return AssetFrame(Path, Asset) ? FTransform(ToUnreal(Asset)) : FTransform::Identity;
}

void
URigExecComponent::LoadTouchRegions()
{
#if USE_USD_SDK
	if (!Touch || !Stage)
	{
		return;
	}
	const pxr::UsdStageRefPtr UsdStage(Stage);
	const pxr::TfToken RegionsType("RigExecTouchRegions"), RegionType("RigExecTouchRegion");
	const pxr::TfToken MeshRel("rigExec:touch:mesh"), LayerName("rigExec:touch:layerName"),
		LayerOrder("rigExec:touch:layerOrder");
	const pxr::TfToken TypedFaces("rigExec:touch:faces"), CustomFaces("touchpose:faces");
	const pxr::TfToken TypedControl("rigExec:touch:control"), CustomControl("touchpose:control");
	const pxr::TfToken TypedPalette("rigExec:touch:palette"), CustomPalette("touchpose:palette");
	const pxr::TfToken Hilight("touchpose:hilight"), OwnColor("touchpose:color");
	const pxr::TfToken LeadColor("touchpose:leadColor"), SelectedColor("touchpose:selectedColor");

	auto IsRegion = [&](const pxr::UsdPrim& Child) {
		return Child.GetTypeName() == RegionType || Child.HasAttribute(CustomFaces);
	};
	auto Targets = [](const pxr::UsdPrim& Prim, const pxr::TfToken& Typed, const pxr::TfToken& Custom) {
		pxr::SdfPathVector Out;
		for (const pxr::TfToken& Name : {Typed, Custom})
		{
			if (const pxr::UsdRelationship Rel = Prim.GetRelationship(Name))
			{
				Rel.GetTargets(&Out);
				if (!Out.empty())
				{
					break;
				}
			}
		}
		return Out;
	};
	auto Color = [](const pxr::UsdPrim& Prim, const pxr::TfToken& Name, const FLinearColor& Fallback) {
		pxr::GfVec3f Value;
		const pxr::UsdAttribute Attr = Prim.GetAttribute(Name);
		return Attr && Attr.Get(&Value) ? FLinearColor(Value[0], Value[1], Value[2]) : Fallback;
	};

	// Every scope, by type, that names a mesh and holds regions.
	struct FLayer
	{
		int32 Order;
		FString Label;
		FString Path;
		FString Mesh;
		pxr::UsdPrim Prim;
	};
	TArray<FLayer> Layers;
	for (const pxr::UsdPrim& Prim : UsdStage->Traverse())
	{
		if (Prim.GetTypeName() != RegionsType)
		{
			continue;
		}
		const pxr::SdfPathVector Mesh = Targets(Prim, MeshRel, pxr::TfToken("touchpose:mesh"));
		bool bHasRegions = false;
		for (const pxr::UsdPrim& Child : Prim.GetChildren())
		{
			bHasRegions |= IsRegion(Child);
		}
		if (Mesh.empty() || !bHasRegions)
		{
			continue;
		}
		FLayer& Layer = Layers.AddDefaulted_GetRef();
		int Order = 0;
		pxr::TfToken Name;
		if (const pxr::UsdAttribute Attr = Prim.GetAttribute(LayerOrder))
		{
			Attr.Get(&Order);
		}
		if (const pxr::UsdAttribute Attr = Prim.GetAttribute(LayerName))
		{
			Attr.Get(&Name);
		}
		Layer.Order = Order;
		Layer.Label = UTF8_TO_TCHAR((Name.IsEmpty() ? Prim.GetName() : Name).GetText());
		Layer.Path = UTF8_TO_TCHAR(Prim.GetPath().GetString().c_str());
		Layer.Mesh = UTF8_TO_TCHAR(Mesh.front().GetString().c_str());
		Layer.Prim = Prim;
	}
	// The layer that opens live: lowest order, then label, then path.
	Layers.Sort([](const FLayer& A, const FLayer& B) {
		return A.Order != B.Order ? A.Order < B.Order : A.Label != B.Label ? A.Label < B.Label : A.Path < B.Path;
	});

	TArray<FString> Missing;
	for (int32 M = 0; M < Touch->Meshes.Num(); ++M)
	{
		FRigExecTouch::FMesh& Touched = Touch->Meshes[M];
		const FLayer* Layer = Layers.FindByPredicate([&](const FLayer& L) { return L.Mesh == Touched.Path; });
		const pxr::UsdGeomMesh UsdMesh(UsdStage->GetPrimAtPath(pxr::SdfPath(TCHAR_TO_UTF8(*Touched.Path))));
		pxr::VtIntArray Counts;
		if (!Layer || !UsdMesh || !UsdMesh.GetFaceVertexCountsAttr().Get(&Counts))
		{
			continue;
		}
		pxr::VtVec3fArray Palette;
		for (const pxr::TfToken& Name : {TypedPalette, CustomPalette})
		{
			if (const pxr::UsdAttribute Attr = Layer->Prim.GetAttribute(Name))
			{
				if (Attr.Get(&Palette) && !Palette.empty())
				{
					break;
				}
			}
		}
		// Every mesh's scope carries the same two state colours; the last
		// one read stands.
		Touch->LeadColor = ScaleTo(Color(Layer->Prim, LeadColor, GDefaultLeadColor), 0.95f);
		Touch->SelectedColor = ScaleTo(Color(Layer->Prim, SelectedColor, GDefaultSelectedColor), 0.62f);

		Touched.FaceRegion.Init(-1, int32(Counts.size()));
		for (const pxr::UsdPrim& Child : Layer->Prim.GetChildren())
		{
			if (!IsRegion(Child))
			{
				continue;
			}
			// A region that selects nothing must not swallow the click.
			const pxr::SdfPathVector Control = Targets(Child, TypedControl, CustomControl);
			const FString ControlPath = Control.empty() ? FString() : UTF8_TO_TCHAR(Control.front().GetString().c_str());
			// Only an animator control can be selected; a rig helper is a null.
			const FControlMap* Map = ControlMaps.FindByPredicate(
				[&ControlPath](const FControlMap& C) { return !C.bNull && C.Path == ControlPath; });
			const FName Element = Map ? Map->Element : NAME_None;
			if (Element.IsNone())
			{
				if (!ControlPath.IsEmpty())
				{
					Missing.Add(UTF8_TO_TCHAR(Child.GetName().GetText()));
				}
				continue;
			}
			pxr::VtIntArray Faces;
			for (const pxr::TfToken& Name : {TypedFaces, CustomFaces})
			{
				if (const pxr::UsdAttribute Attr = Child.GetAttribute(Name))
				{
					if (Attr.Get(&Faces) && !Faces.empty())
					{
						break;
					}
				}
			}
			int Index = 0;
			if (const pxr::UsdAttribute Attr = Child.GetAttribute(Hilight))
			{
				Attr.Get(&Index);
			}
			const int32 Region = Touch->Regions.Num();
			FRigExecTouch::FRegion& Out = Touch->Regions.AddDefaulted_GetRef();
			Out.Mesh = M;
			Out.Name = UTF8_TO_TCHAR(Child.GetName().GetText());
			Out.ControlPath = ControlPath;
			Out.Element = Element;
			const FLinearColor Base = Palette.empty()
			                              ? Color(Child, OwnColor, FLinearColor(1.0f, 0.6f, 0.0f))
			                              : FLinearColor(Palette[size_t(FMath::Max(Index, 0)) % Palette.size()][0],
			                                             Palette[size_t(FMath::Max(Index, 0)) % Palette.size()][1],
			                                             Palette[size_t(FMath::Max(Index, 0)) % Palette.size()][2]);
			Out.HoverColor = ScaleTo(Base, 1.0f);
			for (int Face : Faces)
			{
				if (Face >= 0 && Face < Touched.FaceRegion.Num())
				{
					Touched.FaceRegion[Face] = Region;
					Out.Faces.Add(Face);
				}
			}
		}
		UE_LOG(LogRigExec, Log, TEXT("touch layer %s on %s"), *Layer->Label, *Touched.Path);
	}
	Touch->Written.Init(uint8(TouchStateCount), Touch->Regions.Num());
	UE_LOG(LogRigExec, Log, TEXT("%d touch regions"), Touch->Regions.Num());
	if (Missing.Num() > 0)
	{
		UE_LOG(LogRigExec, Log, TEXT("%d touch regions name no Control Rig control and are skipped: %s"), Missing.Num(),
		       *FString::Join(Missing, TEXT(", ")));
	}
#endif
}

int32
URigExecComponent::GetTouchRegionCount() const
{
	return Touch ? Touch->Regions.Num() : 0;
}

FString
URigExecComponent::GetTouchRegionName(int32 Region) const
{
	return Touch && Touch->Regions.IsValidIndex(Region) ? Touch->Regions[Region].Name : FString();
}

FName
URigExecComponent::GetTouchRegionControl(int32 Region) const
{
	return Touch && Touch->Regions.IsValidIndex(Region) ? Touch->Regions[Region].Element : NAME_None;
}

TArray<int32>
URigExecComponent::GetTouchRegionsOf(const TArray<FName>& Controls) const
{
	TArray<int32> Out;
	for (int32 Region = 0; Touch && Region < Touch->Regions.Num(); ++Region)
	{
		if (Controls.Contains(Touch->Regions[Region].Element))
		{
			Out.Add(Region);
		}
	}
	return Out;
}

int32
URigExecComponent::PickTouchRegion(const FVector& RayOrigin, const FVector& RayDirection, double& OutDistance)
{
	OutDistance = 0.0;
	int32 Best = -1;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 M = 0; Touch && M < Touch->Meshes.Num() && M < Meshes.Num(); ++M)
	{
		FRigExecTouch::FMesh& Touched = Touch->Meshes[M];
		const FDynamicMesh3* Mesh = Meshes[M] ? Meshes[M]->GetMesh() : nullptr;
		if (Touched.FaceRegion.Num() == 0 || !Mesh)
		{
			continue;
		}
		// Built on the posed skin, and only again once the rig has moved it.
		if (!Touched.Tree || Touched.Tree->GetMesh() != Mesh)
		{
			Touched.Tree = MakeUnique<FDynamicMeshAABBTree3>(Mesh, false);
			Touched.bTreeDirty = true;
		}
		if (Touched.bTreeDirty)
		{
			Touched.Tree->Build();
			Touched.bTreeDirty = false;
		}
		const FTransform& ToWorld = Meshes[M]->GetComponentTransform();
		const FRay3d Ray(ToWorld.InverseTransformPosition(RayOrigin),
		                 ToWorld.InverseTransformVector(RayDirection).GetSafeNormal(), true);
		double T = 0.0;
		int Triangle = -1;
		if (!Touched.Tree->FindNearestHitTriangle(Ray, T, Triangle) || !Touched.TriangleFace.IsValidIndex(Triangle))
		{
			continue;
		}
		const double Distance = FVector::Dist(RayOrigin, ToWorld.TransformPosition(Ray.PointAt(T)));
		if (Distance < BestDistance)
		{
			// Unpainted skin still hides what is behind it.
			BestDistance = Distance;
			Best = Touched.FaceRegion[Touched.TriangleFace[Triangle]];
			OutDistance = Distance;
		}
	}
	return Best;
}

void
URigExecComponent::SetTouchHighlight(int32 Hover, int32 Lead, const TArray<int32>& Selected)
{
	if (!Touch)
	{
		return;
	}
	const bool bWasShown = Touch->IsShown();
	if (Touch->Hover == Hover && Touch->Lead == Lead && Touch->Selected == Selected)
	{
		return;
	}
	Touch->Hover = Hover;
	Touch->Lead = Lead;
	Touch->Selected = Selected;
	if (bWasShown || Touch->IsShown())
	{
		RefreshTouchColors();
	}
}

void
URigExecComponent::SetTouchSuspended(bool bSuspended)
{
	if (!Touch || Touch->bSuspended == bSuspended)
	{
		return;
	}
	Touch->bSuspended = bSuspended;
	RefreshTouchColors();
}

bool
URigExecComponent::IsTouchSuspended() const
{
	return Touch && Touch->bSuspended;
}

void
URigExecComponent::RefreshTouchColors()
{
	if (!Touch || Touch->Written.Num() != Touch->Regions.Num())
	{
		return;
	}
	// Each region's state now (suspended: none lit), by priority.
	TArray<uint8> State;
	State.Init(uint8(TouchStateCount), Touch->Regions.Num());
	if (!Touch->bSuspended)
	{
		for (int32 Region : Touch->Selected)
		{
			if (State.IsValidIndex(Region))
			{
				State[Region] = TouchSelected;
			}
		}
		if (State.IsValidIndex(Touch->Lead))
		{
			State[Touch->Lead] = TouchLead;
		}
		if (State.IsValidIndex(Touch->Hover))
		{
			State[Touch->Hover] = TouchHover;
		}
	}
	// Only the regions whose state changed are rewritten, mesh by mesh.
	TArray<TArray<int32>> Changed;
	Changed.SetNum(Meshes.Num());
	bool bAny = false;
	for (int32 Region = 0; Region < State.Num(); ++Region)
	{
		const int32 M = Touch->Regions[Region].Mesh;
		if (State[Region] != Touch->Written[Region] && Changed.IsValidIndex(M))
		{
			Changed[M].Add(Region);
			bAny = true;
		}
	}
	if (!bAny)
	{
		return;
	}
	for (int32 M = 0; M < Meshes.Num(); ++M)
	{
		if (Changed[M].Num() == 0 || !Meshes[M])
		{
			continue;
		}
		const FRigExecTouch::FMesh& Touched = Touch->Meshes[M];
		Meshes[M]->GetDynamicMesh()->EditMesh(
			[&](FDynamicMesh3& Mesh)
			{
				FDynamicMeshColorOverlay* Colors =
					Mesh.HasAttributes() && Mesh.Attributes()->HasPrimaryColors() ? Mesh.Attributes()->PrimaryColors() : nullptr;
				if (!Colors)
				{
					return;
				}
				for (int32 Region : Changed[M])
				{
					const FRigExecTouch::FRegion& R = Touch->Regions[Region];
					const FLinearColor C = State[Region] == TouchHover    ? R.HoverColor
					                       : State[Region] == TouchLead   ? Touch->LeadColor
					                       : State[Region] == TouchSelected ? Touch->SelectedColor
					                                                         : FLinearColor::White;
					const FVector4f Value =
						State[Region] == TouchStateCount ? GNoTint : FVector4f(C.R, C.G, C.B, GTouchOpacity);
					for (int32 Face : R.Faces)
					{
						for (int32 Triangle = Touched.FaceFirstTriangle[Face];
						     Triangle < Touched.FaceFirstTriangle[Face + 1]; ++Triangle)
						{
							const FIndex3i Elements = Colors->GetTriangle(Triangle);
							for (int32 Corner = 0; Corner < 3; ++Corner)
							{
								Colors->SetElement(Elements[Corner], Value);
							}
						}
					}
				}
			},
			EDynamicMeshChangeType::AttributeEdit, EDynamicMeshAttributeChangeFlags::VertexColors,
			/*bDeferChangeEvents*/ SkinDraws(M));
		if (SkinDraws(M))
		{
			Skins[M]->UpdateColors(*Meshes[M]->GetMesh());
		}
		for (int32 Region : Changed[M])
		{
			Touch->Written[Region] = State[Region];
		}
	}
}
