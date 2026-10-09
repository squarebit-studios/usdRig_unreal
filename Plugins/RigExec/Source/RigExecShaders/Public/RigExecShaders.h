// RigExec's global compute shaders.
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/**
 * Normals and high-precision tangents for a RigExec skin from its posed
 * positions: one thread per render vertex (Shaders/Private/RigExecNormals.usf).
 */
class RIGEXECSHADERS_API FRigExecNormalsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRigExecNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FRigExecNormalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<float>, Positions)
		SHADER_PARAMETER_SRV(Buffer<float2>, UVs)
		SHADER_PARAMETER_SRV(Buffer<uint>, Indices)
		SHADER_PARAMETER_SRV(Buffer<uint>, TriOffsets)
		SHADER_PARAMETER_SRV(Buffer<uint>, Tris)
		SHADER_PARAMETER_UAV(RWBuffer<uint>, OutTangents)
		SHADER_PARAMETER(uint32, NumVertices)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr uint32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

namespace RigExecShaders
{
/** Whether the GPU skin can run here: compute at SM5 and the shader compiled.
 * False with a short reason for the user when it cannot. */
RIGEXECSHADERS_API bool CanUseGpuSkin(FString* OutReason = nullptr);
}
