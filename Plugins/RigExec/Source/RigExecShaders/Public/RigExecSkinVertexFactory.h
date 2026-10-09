// The GPU skin's vertex factory: the engine's local vertex factory, plus the
// GPU-skin pass-through velocity path, so a posed RigExec mesh writes motion
// vectors for TSR and motion blur from its current and previous positions.
#pragma once

#include "CoreMinimal.h"
#include "GPUSkinVertexFactory.h"
#include "LocalVertexFactory.h"

/**
 * FLocalVertexFactory with velocity. The velocity shader is the engine's own
 * (LocalVertexFactory.ush, SUPPORT_GPUSKIN_PASSTHROUGH): it reads the current
 * and previous positions from FGPUSkinPassThroughFactoryLooseParameters, and
 * uses the previous ones only on the frame they were written for, so a still
 * pose has no velocity. This factory has its own type and shader parameters to
 * bind those loose parameters (the engine's pass-through factory is not
 * exported) -- the same approach as Squarebit Subdivs. Lives in this
 * PostConfigInit module because vertex factory types must exist before
 * shaders compile.
 */
class RIGEXECSHADERS_API FRigExecSkinVertexFactory : public FLocalVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FRigExecSkinVertexFactory);

public:
	FRigExecSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName)
		: FLocalVertexFactory(InFeatureLevel, InDebugName)
	{
	}

	/** Turns the velocity path on or off; a CPU flag, safe any time. */
	void EnableVelocity(bool bEnable) { bGPUSkinPassThrough = bEnable; }
	bool IsVelocityEnabled() const { return bGPUSkinPassThrough; }
	/** True once real positions have been supplied. */
	bool IsVelocityReady() const { return bVelocityReady; }

	/**
	 * Points the velocity path at this frame's positions: `CurrentPositions`
	 * and `PreviousPositions` are Buffer<float> SRVs, 3 floats per vertex, and
	 * `FrameNumber` the game frame (GFrameCounter) they were posed in. Null
	 * SRVs bind placeholders (no velocity); the shader's slot is always bound.
	 * Render thread.
	 */
	void UpdateVelocity_RenderThread(FRHICommandListBase& RHICmdList, FRHIShaderResourceView* CurrentPositions,
	                                 FRHIShaderResourceView* PreviousPositions, uint32 FrameNumber);

	void ReleaseVelocityResources();

	TUniformBufferRef<FGPUSkinPassThroughFactoryLooseParameters> LooseParameters;

private:
	FBufferRHIRef DummyFloatBuffer;
	FShaderResourceViewRHIRef DummyFloatSRV;
	FBufferRHIRef DummyTangentBuffer;
	FShaderResourceViewRHIRef DummyTangentSRV;
	bool bVelocityReady = false;
};
