#include "RigExecSkinComponent.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "GlobalShader.h"
#include "LocalVertexFactory.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RayTracingInstance.h"
#include "RayTracingGeometry.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RigExecShaders.h"
#include "RigExecSkinVertexFactory.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "StaticMeshResources.h"

using namespace UE::Geometry;

namespace
{
/** A GPU buffer the compute shader reads (and, with bUAV, writes): typed
 * views of `Format`, filled from `Initial` when it is not empty. */
class FRigExecTypedBuffer : public FVertexBuffer
{
public:
	FRigExecTypedBuffer(const TCHAR* InName, EPixelFormat InFormat) : Name(InName), Format(InFormat) {}

	const TCHAR* Name;
	EPixelFormat Format;
	uint32 Bytes = 0;
	bool bUAV = false;
	bool bDynamic = false;
	TArray<uint8> Initial;
	FShaderResourceViewRHIRef SRV;
	FUnorderedAccessViewRHIRef UAV;

	template <typename T>
	void SetData(const TArray<T>& Values)
	{
		Bytes = uint32(Values.Num() * sizeof(T));
		Initial.SetNumUninitialized(Bytes);
		FMemory::Memcpy(Initial.GetData(), Values.GetData(), Bytes);
	}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (Bytes == 0)
		{
			return;
		}
		EBufferUsageFlags Usage = EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::ShaderResource;
		Usage |= bUAV ? EBufferUsageFlags::UnorderedAccess : EBufferUsageFlags::None;
		Usage |= bDynamic ? EBufferUsageFlags::Dynamic : EBufferUsageFlags::Static;
		const FRHIBufferCreateDesc Desc =
			FRHIBufferCreateDesc::CreateVertex(Name, Bytes).AddUsage(Usage).DetermineInitialState();
		VertexBufferRHI = RHICmdList.CreateBuffer(Desc);
		if (Initial.Num() == int32(Bytes))
		{
			void* Mapped = RHICmdList.LockBuffer(VertexBufferRHI, 0, Bytes, RLM_WriteOnly);
			FMemory::Memcpy(Mapped, Initial.GetData(), Bytes);
			RHICmdList.UnlockBuffer(VertexBufferRHI);
			Initial.Empty();
		}
		SRV = RHICmdList.CreateShaderResourceView(
			VertexBufferRHI, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(Format));
		if (bUAV)
		{
			UAV = RHICmdList.CreateUnorderedAccessView(
				VertexBufferRHI, FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(Format));
		}
	}

	virtual void ReleaseRHI() override
	{
		UAV.SafeRelease();
		SRV.SafeRelease();
		FVertexBuffer::ReleaseRHI();
	}
};

class FRigExecSkinProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FRigExecSkinProxy(URigExecSkinComponent* Component, TSharedPtr<const URigExecSkinComponent::FSkinData, ESPMode::ThreadSafe> InData,
	                  const TArray<FVector3f>& Positions, const TArray<FColor>& Colors)
		: FPrimitiveSceneProxy(Component)
		, Data(MoveTemp(InData))
		, VertexFactory(GetScene().GetFeatureLevel(), "FRigExecSkinVertexFactory")
		, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetShaderPlatform()))
		, InputPositions(TEXT("RigExecSkinPositions"), PF_R32_FLOAT)
		, PreviousPositions(TEXT("RigExecSkinPreviousPositions"), PF_R32_FLOAT)
		, UVs(TEXT("RigExecSkinUVs"), PF_G32R32F)
		, Indices(TEXT("RigExecSkinIndices"), PF_R32_UINT)
		, TriOffsets(TEXT("RigExecSkinTriOffsets"), PF_R32_UINT)
		, Tris(TEXT("RigExecSkinTris"), PF_R32_UINT)
		, OutTangents(TEXT("RigExecSkinTangents"), PF_R32_UINT)
	{
		// Deformed every pose: shadows and caches must not keep the old shape.
		bHasDeformableMesh = true;
		ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Auto;
		NumVertices = Positions.Num();
		// Motion vectors from the deformation. The transform never moves, so the
		// renderer would skip the velocity pass: always draw velocity, and let
		// the pass-through path give the per-vertex motion (zero while still).
		bVelocity = Component->bMotionBlur;
		VertexFactory.EnableVelocity(bVelocity);
		bAlwaysHasVelocity = bVelocity;

		TArray<FDynamicMeshVertex> Vertices;
		Vertices.SetNum(NumVertices);
		for (int32 V = 0; V < NumVertices; ++V)
		{
			// Tangents are placeholders: the first dispatch below writes the real ones.
			Vertices[V] = FDynamicMeshVertex(Positions[V], FVector3f(1, 0, 0), FVector3f(0, 0, 1),
			                                 Data->UVs.IsValidIndex(V) ? Data->UVs[V] : FVector2f::ZeroVector,
			                                 Colors.IsValidIndex(V) ? Colors[V] : FColor::White);
		}
		// Full-precision UVs and 16-bit tangents: the compute shader writes that
		// layout, and 8-bit tangents band visibly on a smooth skin.
		VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
		VertexBuffers.StaticMeshVertexBuffer.SetUseHighPrecisionTangentBasis(true);
		VertexBuffers.InitFromDynamicVertex(&VertexFactory, Vertices, 1);
		BeginInitResource(&VertexBuffers.PositionVertexBuffer);
		BeginInitResource(&VertexBuffers.StaticMeshVertexBuffer);
		BeginInitResource(&VertexBuffers.ColorVertexBuffer);
		BeginInitResource(&VertexFactory);

		for (int32 Slot = 0; Slot < Data->SectionIndices.Num(); ++Slot)
		{
			if (Data->SectionIndices[Slot].Num() == 0)
			{
				continue;
			}
			FSection* Section = new FSection;
			Section->IndexBuffer.Indices = Data->SectionIndices[Slot];
			Section->Material = Component->GetMaterial(Slot);
			if (!Section->Material)
			{
				Section->Material = UMaterial::GetDefaultMaterial(MD_Surface);
			}
			BeginInitResource(&Section->IndexBuffer);
			Sections.Add(Section);
		}
#if RHI_RAYTRACING
		// Ray tracing (HW shadows, reflections, Lumen's hardware path): one
		// BLAS per section, built once, refit after every pose.
		if (IsRayTracingEnabled())
		{
			FPositionVertexBuffer* PositionVB = &VertexBuffers.PositionVertexBuffer;
			const FName DebugName = Component->GetFName();
			const int32 VertexCount = NumVertices;
			for (FSection* Section : Sections)
			{
				ENQUEUE_RENDER_COMMAND(RigExecSkinInitRayTracing)(
					[Section, PositionVB, DebugName, VertexCount](FRHICommandListImmediate& RHICmdList)
					{
						const int32 Primitives = Section->IndexBuffer.Indices.Num() / 3;
						if (Primitives == 0 || !PositionVB->VertexBufferRHI.IsValid() || !Section->IndexBuffer.IndexBufferRHI.IsValid())
						{
							return;
						}
						FRayTracingGeometryInitializer Initializer;
						Initializer.DebugName = DebugName;
						Initializer.IndexBuffer = Section->IndexBuffer.IndexBufferRHI;
						Initializer.TotalPrimitiveCount = Primitives;
						Initializer.GeometryType = RTGT_Triangles;
						Initializer.bFastBuild = true;
						// Refit in place each pose: ~10x cheaper than a rebuild.
						Initializer.bAllowUpdate = true;
						FRayTracingGeometrySegment Segment;
						Segment.VertexBuffer = PositionVB->VertexBufferRHI;
						Segment.VertexBufferStride = PositionVB->GetStride();
						Segment.MaxVertices = VertexCount;
						Segment.NumPrimitives = Primitives;
						Initializer.Segments.Add(Segment);
						Section->RayTracingGeometry.SetInitializer(Initializer);
						Section->RayTracingGeometry.InitResource(RHICmdList);
						// The one full build; updates refit it from here on.
						FRayTracingGeometryBuildParams Params;
						Params.Geometry = Section->RayTracingGeometry.GetRHI();
						Params.BuildMode = EAccelerationStructureBuildMode::Build;
						Params.Segments = Section->RayTracingGeometry.GetInitializer().Segments;
						RHICmdList.BuildAccelerationStructures(MakeArrayView(&Params, 1));
					});
			}
		}
#endif

		// The compute shader's inputs: the uploaded positions, and the static
		// topology it walks.
		InputPositions.Bytes = uint32(NumVertices * sizeof(FVector3f));
		InputPositions.bDynamic = true;
		PreviousPositions.Bytes = InputPositions.Bytes;
		UVs.SetData(Data->UVs);
		Indices.SetData(Data->Indices);
		TriOffsets.SetData(Data->TriOffsets);
		Tris.SetData(Data->Tris);
		OutTangents.Bytes = uint32(NumVertices * 4 * sizeof(uint32));
		OutTangents.bUAV = true;
		for (FRigExecTypedBuffer* Buffer : {&InputPositions, &PreviousPositions, &UVs, &Indices, &TriOffsets, &Tris, &OutTangents})
		{
			BeginInitResource(Buffer);
		}

		// The velocity slot is bound from the first draw (placeholders), then
		// the first pose: positions are already in the vertex buffer; this
		// gives them their normals, and a previous pose equal to it.
		FRigExecSkinProxy* Self = this;
		const uint32 FrameNumber = uint32(GFrameCounter);
		ENQUEUE_RENDER_COMMAND(RigExecSkinFirstPose)(
			[Self, Positions, FrameNumber](FRHICommandListImmediate& RHICmdList)
			{
				Self->VertexFactory.UpdateVelocity_RenderThread(RHICmdList, nullptr, nullptr, 0);
				Self->UpdatePositions_RenderThread(RHICmdList, Positions, FrameNumber);
			});
	}

	virtual ~FRigExecSkinProxy() override
	{
		VertexBuffers.PositionVertexBuffer.ReleaseResource();
		VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
		VertexBuffers.ColorVertexBuffer.ReleaseResource();
		VertexFactory.ReleaseVelocityResources();
		VertexFactory.ReleaseResource();
		for (FRigExecTypedBuffer* Buffer : {&InputPositions, &PreviousPositions, &UVs, &Indices, &TriOffsets, &Tris, &OutTangents})
		{
			Buffer->ReleaseResource();
		}
		for (FSection* Section : Sections)
		{
#if RHI_RAYTRACING
			Section->RayTracingGeometry.ReleaseResource();
#endif
			Section->IndexBuffer.ReleaseResource();
			delete Section;
		}
	}

	/** Uploads a pose posed in game frame `FrameNumber`: positions to the GPU
	 * once, normals and tangents computed there, both copied into the vertex
	 * factory's buffers; the pose before it kept for the motion vectors. */
	void UpdatePositions_RenderThread(FRHICommandListImmediate& RHICmdList, const TArray<FVector3f>& Positions, uint32 FrameNumber)
	{
		FRHIBuffer* PositionVB = VertexBuffers.PositionVertexBuffer.VertexBufferRHI;
		FRHIBuffer* TangentVB = VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.VertexBufferRHI;
		if (Positions.Num() != NumVertices || NumVertices == 0 || !InputPositions.VertexBufferRHI || !PositionVB ||
		    !TangentVB || !OutTangents.UAV)
		{
			return;
		}
		const uint32 PositionBytes = InputPositions.Bytes;
		// The pose being replaced becomes the previous one (motion vectors).
		const bool bKeepPrevious = bVelocity && bHasPose && PreviousPositions.VertexBufferRHI;
		if (bKeepPrevious)
		{
			RHICmdList.Transition({
				FRHITransitionInfo(InputPositions.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::CopySrc),
				FRHITransitionInfo(PreviousPositions.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::CopyDest),
			});
			RHICmdList.CopyBufferRegion(PreviousPositions.VertexBufferRHI, 0, InputPositions.VertexBufferRHI, 0, PositionBytes);
		}
		void* Mapped = RHICmdList.LockBuffer(InputPositions.VertexBufferRHI, 0, PositionBytes, RLM_WriteOnly);
		FMemory::Memcpy(Mapped, Positions.GetData(), PositionBytes);
		RHICmdList.UnlockBuffer(InputPositions.VertexBufferRHI);
		// The first pose has no predecessor: previous equals current, no motion.
		if (bVelocity && !bHasPose && PreviousPositions.VertexBufferRHI)
		{
			RHICmdList.Transition({
				FRHITransitionInfo(InputPositions.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::CopySrc),
				FRHITransitionInfo(PreviousPositions.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::CopyDest),
			});
			RHICmdList.CopyBufferRegion(PreviousPositions.VertexBufferRHI, 0, InputPositions.VertexBufferRHI, 0, PositionBytes);
		}
		bHasPose = true;

		// Positions: straight into the vertex factory's buffer.
		RHICmdList.Transition({
			FRHITransitionInfo(InputPositions.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::CopySrc | ERHIAccess::SRVCompute),
			FRHITransitionInfo(PositionVB, ERHIAccess::Unknown, ERHIAccess::CopyDest),
			FRHITransitionInfo(OutTangents.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::UAVCompute),
		});
		RHICmdList.CopyBufferRegion(PositionVB, 0, InputPositions.VertexBufferRHI, 0, PositionBytes);

		// Normals and tangents from them.
		FRigExecNormalsCS::FParameters Parameters;
		Parameters.Positions = InputPositions.SRV;
		Parameters.UVs = UVs.SRV;
		Parameters.Indices = Indices.SRV;
		Parameters.TriOffsets = TriOffsets.SRV;
		Parameters.Tris = Tris.SRV;
		Parameters.OutTangents = OutTangents.UAV;
		Parameters.NumVertices = uint32(NumVertices);
		TShaderMapRef<FRigExecNormalsCS> Shader(GetGlobalShaderMap(GetScene().GetFeatureLevel()));
		FComputeShaderUtils::Dispatch(RHICmdList, Shader, Parameters,
		                              FComputeShaderUtils::GetGroupCount(NumVertices, FRigExecNormalsCS::ThreadGroupSize));

		RHICmdList.Transition({
			FRHITransitionInfo(OutTangents.VertexBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc),
			FRHITransitionInfo(TangentVB, ERHIAccess::Unknown, ERHIAccess::CopyDest),
		});
		RHICmdList.CopyBufferRegion(TangentVB, 0, OutTangents.VertexBufferRHI, 0, OutTangents.Bytes);
		RHICmdList.Transition({
			FRHITransitionInfo(PositionVB, ERHIAccess::CopyDest, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask),
			FRHITransitionInfo(TangentVB, ERHIAccess::CopyDest, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask),
			FRHITransitionInfo(InputPositions.VertexBufferRHI, ERHIAccess::CopySrc | ERHIAccess::SRVCompute, ERHIAccess::SRVMask),
			FRHITransitionInfo(OutTangents.VertexBufferRHI, ERHIAccess::CopySrc, ERHIAccess::UAVCompute),
		});
#if RHI_RAYTRACING
		// Refit the BLAS to the pose just copied in.
		if (IsRayTracingEnabled())
		{
			TArray<FRayTracingGeometryBuildParams> Builds;
			for (FSection* Section : Sections)
			{
				if (Section->RayTracingGeometry.IsValid() && Section->RayTracingGeometry.GetRHI())
				{
					FRayTracingGeometryBuildParams& Params = Builds.AddDefaulted_GetRef();
					Params.Geometry = Section->RayTracingGeometry.GetRHI();
					Params.BuildMode = EAccelerationStructureBuildMode::Update;
					Params.Segments = Section->RayTracingGeometry.GetInitializer().Segments;
				}
			}
			if (Builds.Num() > 0)
			{
				RHICmdList.BuildAccelerationStructures(Builds);
			}
		}
#endif
		if (bVelocity)
		{
			// The velocity shader reads both poses in the vertex shader, and
			// only in the frame they were posed in: still poses get no motion.
			RHICmdList.Transition(FRHITransitionInfo(PreviousPositions.VertexBufferRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
			VertexFactory.UpdateVelocity_RenderThread(RHICmdList, InputPositions.SRV, PreviousPositions.SRV, FrameNumber);
		}
	}

	void UpdateColors_RenderThread(FRHICommandListImmediate& RHICmdList, const TArray<FColor>& Colors)
	{
		FRHIBuffer* ColorVB = VertexBuffers.ColorVertexBuffer.VertexBufferRHI;
		if (Colors.Num() != NumVertices || !ColorVB)
		{
			return;
		}
		const uint32 Bytes = uint32(NumVertices * sizeof(FColor));
		void* Mapped = RHICmdList.LockBuffer(ColorVB, 0, Bytes, RLM_WriteOnly);
		FMemory::Memcpy(Mapped, Colors.GetData(), Bytes);
		RHICmdList.UnlockBuffer(ColorVB);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
	                                    uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
		FColoredMaterialRenderProxy* WireframeMaterial = nullptr;
		if (bWireframe)
		{
			WireframeMaterial = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr, FLinearColor(0, 0.5f, 1.f));
			Collector.RegisterOneFrameMaterialProxy(WireframeMaterial);
		}
		for (const FSection* Section : Sections)
		{
			FMaterialRenderProxy* Material = bWireframe ? WireframeMaterial : Section->Material->GetRenderProxy();
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (!(VisibilityMap & (1 << ViewIndex)))
				{
					continue;
				}
				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& Element = Mesh.Elements[0];
				Element.IndexBuffer = &Section->IndexBuffer;
				Mesh.bWireframe = bWireframe;
				Mesh.VertexFactory = &VertexFactory;
				Mesh.MaterialRenderProxy = Material;

				FDynamicPrimitiveUniformBuffer& Uniforms = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
				FPrimitiveUniformShaderParametersBuilder Builder;
				BuildUniformShaderParameters(Builder);
				Uniforms.Set(Collector.GetRHICommandList(), Builder);
				Element.PrimitiveUniformBufferResource = &Uniforms.UniformBuffer;

				Element.FirstIndex = 0;
				Element.NumPrimitives = Section->IndexBuffer.Indices.Num() / 3;
				Element.MinVertexIndex = 0;
				Element.MaxVertexIndex = NumVertices - 1;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = false;
				Collector.AddMesh(ViewIndex, Mesh);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	virtual bool CanBeOccluded() const override { return !MaterialRelevance.bDisableDepthTest; }

#if RHI_RAYTRACING
	virtual bool IsRayTracingRelevant() const override { return true; }
	virtual bool HasRayTracingRepresentation() const override { return true; }

	virtual void GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector) override
	{
		TConstArrayView<const FSceneView*> Views = Collector.GetViews();
		const uint32 VisibilityMap = Collector.GetVisibilityMap();
		const int32 FirstView = FMath::CountTrailingZeros(VisibilityMap);
		if (!Views.IsValidIndex(FirstView))
		{
			return;
		}
		for (const FSection* Section : Sections)
		{
			if (!Section->RayTracingGeometry.IsValid() || !Section->RayTracingGeometry.GetRHI())
			{
				continue;
			}
			FRayTracingInstance Instance;
			Instance.Geometry = &Section->RayTracingGeometry;
			Instance.InstanceTransforms.Add(GetLocalToWorld());

			FMeshBatch Mesh;
			Mesh.VertexFactory = &VertexFactory;
			Mesh.SegmentIndex = 0;
			Mesh.MaterialRenderProxy = Section->Material->GetRenderProxy();
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Mesh.CastRayTracedShadow = IsShadowCast(Views[FirstView]);
			FMeshBatchElement& Element = Mesh.Elements[0];
			Element.IndexBuffer = &Section->IndexBuffer;
			FDynamicPrimitiveUniformBuffer& Uniforms = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
			FPrimitiveUniformShaderParametersBuilder Builder;
			BuildUniformShaderParameters(Builder);
			Uniforms.Set(Collector.GetRHICommandList(), Builder);
			Element.PrimitiveUniformBufferResource = &Uniforms.UniformBuffer;
			Element.FirstIndex = 0;
			Element.NumPrimitives = Section->IndexBuffer.Indices.Num() / 3;
			Element.MinVertexIndex = 0;
			Element.MaxVertexIndex = NumVertices - 1;
			Instance.Materials.Add(Mesh);

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if (VisibilityMap & (1 << ViewIndex))
				{
					Collector.AddRayTracingInstance(ViewIndex, Instance);
				}
			}
		}
	}
#endif
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	struct FSection
	{
		FDynamicMeshIndexBuffer32 IndexBuffer;
		UMaterialInterface* Material = nullptr;
#if RHI_RAYTRACING
		FRayTracingGeometry RayTracingGeometry;
#endif
	};

	TSharedPtr<const URigExecSkinComponent::FSkinData, ESPMode::ThreadSafe> Data;
	FStaticMeshVertexBuffers VertexBuffers;
	FRigExecSkinVertexFactory VertexFactory;
	FMaterialRelevance MaterialRelevance;
	TArray<FSection*> Sections;
	int32 NumVertices = 0;
	bool bVelocity = false;
	bool bHasPose = false;
	FRigExecTypedBuffer InputPositions;
	FRigExecTypedBuffer PreviousPositions;
	FRigExecTypedBuffer UVs;
	FRigExecTypedBuffer Indices;
	FRigExecTypedBuffer TriOffsets;
	FRigExecTypedBuffer Tris;
	FRigExecTypedBuffer OutTangents;
};
} // namespace

URigExecSkinComponent::URigExecSkinComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bUseAsOccluder = false;
}

void
URigExecSkinComponent::Build(const FDynamicMesh3& Mesh, const TArray<UMaterialInterface*>& Materials)
{
	SlotMaterials.Reset();
	for (UMaterialInterface* Material : Materials)
	{
		SlotMaterials.Add(Material);
	}
	const int32 NumSlots = FMath::Max(Materials.Num(), 1);
	const FDynamicMeshAttributeSet* Attributes = Mesh.HasAttributes() ? Mesh.Attributes() : nullptr;
	const FDynamicMeshUVOverlay* UVOverlay = Attributes && Attributes->NumUVLayers() > 0 ? Attributes->PrimaryUV() : nullptr;
	const FDynamicMeshMaterialAttribute* MaterialIDs = Attributes && Attributes->HasMaterialID() ? Attributes->GetMaterialID() : nullptr;

	TSharedPtr<FSkinData, ESPMode::ThreadSafe> NewData = MakeShared<FSkinData, ESPMode::ThreadSafe>();
	NewData->SectionIndices.SetNum(NumSlots);
	RenderVertex.Reset();
	CornerRender.Init(-1, Mesh.MaxTriangleID() * 3);

	// A render vertex per (point, UV element, polygon): UV seams stay seams,
	// and each polygon owns its colour. The polygon is the triangle group.
	TMap<FIntVector, int32> RenderOf;
	TArray<int32> CompactTriangle;
	CompactTriangle.Init(-1, Mesh.MaxTriangleID());
	for (int32 Tid : Mesh.TriangleIndicesItr())
	{
		const FIndex3i T = Mesh.GetTriangle(Tid);
		const FIndex3i UVElements = UVOverlay ? UVOverlay->GetTriangle(Tid) : FIndex3i(-1, -1, -1);
		const int32 Group = Mesh.HasTriangleGroups() ? Mesh.GetTriangleGroup(Tid) : Tid;
		CompactTriangle[Tid] = NewData->Indices.Num() / 3;
		const int32 Slot = FMath::Clamp(MaterialIDs ? MaterialIDs->GetValue(Tid) : 0, 0, NumSlots - 1);
		for (int32 C = 0; C < 3; ++C)
		{
			const FIntVector Key(T[C], UVElements[C], Group);
			int32 Render;
			if (const int32* Found = RenderOf.Find(Key))
			{
				Render = *Found;
			}
			else
			{
				Render = RenderVertex.Num();
				RenderOf.Add(Key, Render);
				RenderVertex.Add(T[C]);
				NewData->UVs.Add(UVOverlay && UVOverlay->IsElement(UVElements[C]) ? UVOverlay->GetElement(UVElements[C]) : FVector2f::ZeroVector);
			}
			CornerRender[Tid * 3 + C] = Render;
			NewData->Indices.Add(uint32(Render));
			NewData->SectionIndices[Slot].Add(uint32(Render));
		}
	}

	// Per render vertex, the triangles around its point: the normal sums them
	// all, so it is the same on every split of the point.
	TArray<int32> PointStart, PointTris;
	PointStart.Init(0, Mesh.MaxVertexID() + 1);
	for (int32 Tid : Mesh.TriangleIndicesItr())
	{
		const FIndex3i T = Mesh.GetTriangle(Tid);
		for (int32 C = 0; C < 3; ++C)
		{
			++PointStart[T[C] + 1];
		}
	}
	for (int32 V = 0; V < Mesh.MaxVertexID(); ++V)
	{
		PointStart[V + 1] += PointStart[V];
	}
	PointTris.SetNumUninitialized(PointStart.Last());
	{
		TArray<int32> Fill = PointStart;
		for (int32 Tid : Mesh.TriangleIndicesItr())
		{
			const FIndex3i T = Mesh.GetTriangle(Tid);
			for (int32 C = 0; C < 3; ++C)
			{
				PointTris[Fill[T[C]]++] = CompactTriangle[Tid];
			}
		}
	}
	NewData->TriOffsets.SetNumUninitialized(RenderVertex.Num() + 1);
	NewData->TriOffsets[0] = 0;
	for (int32 R = 0; R < RenderVertex.Num(); ++R)
	{
		const int32 V = RenderVertex[R];
		for (int32 I = PointStart[V]; I < PointStart[V + 1]; ++I)
		{
			NewData->Tris.Add(uint32(PointTris[I]));
		}
		NewData->TriOffsets[R + 1] = uint32(NewData->Tris.Num());
	}

	Data = NewData;
	GatherPositions(Mesh);
	GatherColors(Mesh);
	UpdateBounds();
	MarkRenderStateDirty();
}

void
URigExecSkinComponent::GatherPositions(const FDynamicMesh3& Mesh)
{
	Positions.SetNumUninitialized(RenderVertex.Num());
	FBox3f Box(ForceInit);
	for (int32 R = 0; R < RenderVertex.Num(); ++R)
	{
		Positions[R] = Mesh.IsVertex(RenderVertex[R]) ? FVector3f(Mesh.GetVertex(RenderVertex[R])) : FVector3f::ZeroVector;
		Box += Positions[R];
	}
	LocalBounds = FBoxSphereBounds(FBox(Box));
}

void
URigExecSkinComponent::GatherColors(const FDynamicMesh3& Mesh)
{
	const FDynamicMeshColorOverlay* ColorOverlay =
		Mesh.HasAttributes() && Mesh.Attributes()->HasPrimaryColors() ? Mesh.Attributes()->PrimaryColors() : nullptr;
	Colors.Init(FColor::White, RenderVertex.Num());
	if (!ColorOverlay)
	{
		return;
	}
	for (int32 Tid : Mesh.TriangleIndicesItr())
	{
		const FIndex3i Elements = ColorOverlay->GetTriangle(Tid);
		for (int32 C = 0; C < 3; ++C)
		{
			const int32 R = CornerRender.IsValidIndex(Tid * 3 + C) ? CornerRender[Tid * 3 + C] : -1;
			if (R >= 0 && ColorOverlay->IsElement(Elements[C]))
			{
				const FVector4f V = ColorOverlay->GetElement(Elements[C]);
				// As the Dynamic Mesh renders it (no colour-space transform).
				Colors[R] = FLinearColor(V.X, V.Y, V.Z, V.W).ToFColor(false);
			}
		}
	}
}

void
URigExecSkinComponent::UpdatePositions(const FDynamicMesh3& Mesh)
{
	if (!Data)
	{
		return;
	}
	GatherPositions(Mesh);
	UpdateBounds();
	if (FRigExecSkinProxy* Proxy = static_cast<FRigExecSkinProxy*>(SceneProxy))
	{
		// The game frame the pose belongs to: the velocity shader matches it
		// against the view's frame counter.
		const uint32 FrameNumber = uint32(GFrameCounter);
		ENQUEUE_RENDER_COMMAND(RigExecSkinPose)(
			[Proxy, Positions = Positions, FrameNumber](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->UpdatePositions_RenderThread(RHICmdList, Positions, FrameNumber);
			});
		// New bounds, and shadow caches told the shape moved.
		MarkRenderTransformDirty();
	}
}

void
URigExecSkinComponent::UpdateColors(const FDynamicMesh3& Mesh)
{
	if (!Data)
	{
		return;
	}
	GatherColors(Mesh);
	if (FRigExecSkinProxy* Proxy = static_cast<FRigExecSkinProxy*>(SceneProxy))
	{
		ENQUEUE_RENDER_COMMAND(RigExecSkinColors)(
			[Proxy, Colors = Colors](FRHICommandListImmediate& RHICmdList) { Proxy->UpdateColors_RenderThread(RHICmdList, Colors); });
	}
}

FPrimitiveSceneProxy*
URigExecSkinComponent::CreateSceneProxy()
{
	if (!Data || Positions.Num() == 0 || Data->Indices.Num() == 0)
	{
		return nullptr;
	}
	return new FRigExecSkinProxy(this, Data, Positions, Colors);
}

FBoxSphereBounds
URigExecSkinComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return LocalBounds.TransformBy(LocalToWorld);
}

UMaterialInterface*
URigExecSkinComponent::GetMaterial(int32 Index) const
{
	if (UMaterialInterface* Override = Super::GetMaterial(Index))
	{
		return Override;
	}
	return SlotMaterials.IsValidIndex(Index) ? SlotMaterials[Index].Get() : nullptr;
}

void
URigExecSkinComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	for (int32 Index = 0; Index < GetNumMaterials(); ++Index)
	{
		if (UMaterialInterface* Material = GetMaterial(Index))
		{
			OutMaterials.Add(Material);
		}
	}
}
