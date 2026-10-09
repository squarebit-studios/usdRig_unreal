#include "RigExecSkinVertexFactory.h"

#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"

/** FLocalVertexFactory's parameters, plus the pass-through loose parameters
 * the velocity shader reads, bound from our factory. */
class FRigExecSkinVFShaderParameters : public FLocalVertexFactoryShaderParametersBase
{
	DECLARE_TYPE_LAYOUT(FRigExecSkinVFShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);
		IsGPUSkinPassThrough.Bind(ParameterMap, TEXT("bIsGPUSkinPassThrough"));
	}

	void GetElementShaderBindings(const FSceneInterface* Scene, const FSceneView* View, const FMeshMaterialShader* Shader,
	                              const EVertexInputStreamType InputStreamType, ERHIFeatureLevel::Type FeatureLevel,
	                              const FVertexFactory* VertexFactory, const FMeshBatchElement& BatchElement,
	                              FMeshDrawSingleShaderBindings& ShaderBindings, FVertexInputStreamArray& VertexStreams) const
	{
		const FRigExecSkinVertexFactory* Factory = static_cast<const FRigExecSkinVertexFactory*>(VertexFactory);
		FRHIUniformBuffer* VFUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
		GetElementShaderBindingsBase(Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement,
		                             VFUniformBuffer, ShaderBindings, VertexStreams);
		// The pass-through shader references this slot whatever the flag says,
		// so it is always bound (placeholders until the first pose).
		if (Factory->LooseParameters.IsValid())
		{
			ShaderBindings.Add(Shader->GetUniformBufferParameter<FGPUSkinPassThroughFactoryLooseParameters>(),
			                   Factory->LooseParameters);
		}
		const bool bUse = Factory->IsVelocityEnabled() && Factory->IsVelocityReady();
		ShaderBindings.Add(IsGPUSkinPassThrough, uint32(bUse ? 1 : 0));
	}

	LAYOUT_FIELD(FShaderParameter, IsGPUSkinPassThrough);
};

IMPLEMENT_TYPE_LAYOUT(FRigExecSkinVFShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FRigExecSkinVertexFactory, SF_Vertex, FRigExecSkinVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FRigExecSkinVertexFactory, SF_RayHitGroup, FRigExecSkinVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FRigExecSkinVertexFactory, SF_Compute, FRigExecSkinVFShaderParameters);

// The local factory's own flags, the pass-through among them so the velocity
// permutation compiles for this type.
IMPLEMENT_VERTEX_FACTORY_TYPE(FRigExecSkinVertexFactory, "/Engine/Private/LocalVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsStaticLighting
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsPositionOnly
	| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
	| EVertexFactoryFlags::SupportsRayTracing
	| EVertexFactoryFlags::SupportsRayTracingDynamicGeometry
	| EVertexFactoryFlags::SupportsLightmapBaking
	| EVertexFactoryFlags::SupportsManualVertexFetch
	| EVertexFactoryFlags::SupportsPSOPrecaching
	| EVertexFactoryFlags::SupportsGPUSkinPassThrough
	| EVertexFactoryFlags::SupportsLumenMeshCards
	| EVertexFactoryFlags::SupportsTriangleSorting
);

void
FRigExecSkinVertexFactory::UpdateVelocity_RenderThread(FRHICommandListBase& RHICmdList, FRHIShaderResourceView* CurrentPositions,
                                                       FRHIShaderResourceView* PreviousPositions, uint32 FrameNumber)
{
	// Placeholders, so the loose parameters are always bindable.
	if (!DummyFloatSRV.IsValid())
	{
		const FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(TEXT("RigExecSkinDummyFloat"), sizeof(float) * 4)
		                                      .AddUsage(EBufferUsageFlags::ShaderResource | EBufferUsageFlags::Static)
		                                      .DetermineInitialState();
		DummyFloatBuffer = RHICmdList.CreateBuffer(Desc);
		void* Mapped = RHICmdList.LockBuffer(DummyFloatBuffer, 0, sizeof(float) * 4, RLM_WriteOnly);
		FMemory::Memzero(Mapped, sizeof(float) * 4);
		RHICmdList.UnlockBuffer(DummyFloatBuffer);
		DummyFloatSRV = RHICmdList.CreateShaderResourceView(
			DummyFloatBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_FLOAT));
	}
	if (!DummyTangentSRV.IsValid())
	{
		const FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(TEXT("RigExecSkinDummyTangent"), sizeof(FVector4f))
		                                      .AddUsage(EBufferUsageFlags::ShaderResource | EBufferUsageFlags::Static)
		                                      .DetermineInitialState();
		DummyTangentBuffer = RHICmdList.CreateBuffer(Desc);
		void* Mapped = RHICmdList.LockBuffer(DummyTangentBuffer, 0, sizeof(FVector4f), RLM_WriteOnly);
		FMemory::Memzero(Mapped, sizeof(FVector4f));
		RHICmdList.UnlockBuffer(DummyTangentBuffer);
		DummyTangentSRV = RHICmdList.CreateShaderResourceView(
			DummyTangentBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_A32B32G32R32F));
	}
	const bool bReal = CurrentPositions && PreviousPositions;
	FGPUSkinPassThroughFactoryLooseParameters Parameters;
	Parameters.FrameNumber = FrameNumber;
	Parameters.PositionBuffer = bReal ? CurrentPositions : DummyFloatSRV.GetReference();
	Parameters.PreviousPositionBuffer = bReal ? PreviousPositions : DummyFloatSRV.GetReference();
	Parameters.PreSkinnedTangentBuffer = DummyTangentSRV;
	LooseParameters = TUniformBufferRef<FGPUSkinPassThroughFactoryLooseParameters>::CreateUniformBufferImmediate(
		Parameters, UniformBuffer_MultiFrame);
	bVelocityReady = bReal;
}

void
FRigExecSkinVertexFactory::ReleaseVelocityResources()
{
	LooseParameters.SafeRelease();
	DummyTangentSRV.SafeRelease();
	DummyTangentBuffer.SafeRelease();
	DummyFloatSRV.SafeRelease();
	DummyFloatBuffer.SafeRelease();
	bVelocityReady = false;
}
