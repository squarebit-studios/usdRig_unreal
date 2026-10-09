#include "RigExecShaders.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "ShaderCore.h"

IMPLEMENT_GLOBAL_SHADER(FRigExecNormalsCS, "/Plugin/RigExec/Private/RigExecNormals.usf", "MainCS", SF_Compute);

static TAutoConsoleVariable<int32> CVarRigExecForceCPUFallback(
	TEXT("RigExec.ForceCPUFallback"), 0,
	TEXT("1: RigExec characters loaded from now on draw on the CPU fallback, as if the GPU path could not run, ")
	TEXT("so the fallback, its toast and its Details banner can be seen and tested. Reload a character to apply."),
	ECVF_Default);

bool
RigExecShaders::CanUseGpuSkin(FString* OutReason)
{
	auto Fail = [OutReason](const TCHAR* Why)
	{
		if (OutReason)
		{
			*OutReason = Why;
		}
		return false;
	};
	if (CVarRigExecForceCPUFallback.GetValueOnGameThread() != 0)
	{
		return Fail(TEXT("the GPU path is switched off for testing (RigExec.ForceCPUFallback 1)"));
	}
	if (!GDynamicRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{
		return Fail(TEXT("this renderer has no compute shaders (it runs below Shader Model 5)"));
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!ShaderMap || !TShaderMapRef<FRigExecNormalsCS>(ShaderMap).IsValid())
	{
		return Fail(TEXT("the RigExec normals shader is not compiled for this platform"));
	}
	return true;
}

class FRigExecShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RigExec"));
		if (Plugin)
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/RigExec"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};

IMPLEMENT_MODULE(FRigExecShadersModule, RigExecShaders)
