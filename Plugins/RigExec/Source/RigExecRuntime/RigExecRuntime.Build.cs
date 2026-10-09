// The RigExec binary runtime inside Unreal: the rig is posed by the
// vendored rigExecRuntime/rigExecBinary libraries (ThirdParty/RigExecLib),
// and each mesh's topology is read once from the USD stage.
using UnrealBuildTool;

public class RigExecRuntime : ModuleRules
{
	public RigExecRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "UnrealUSDWrapper", "ControlRig", "RigVM" });
		// Meshes are drawn as dynamic meshes built from the stage's own
		// topology, so vertex i is the rig's point i.
		PrivateDependencyModuleNames.AddRange(new string[] {
			"GeometryCore", "GeometryFramework", "USDUtilities", "USDClasses", "Json", "RigExecLib",
			// The GPU skin: its vertex factory, buffers and compute shader.
			"RenderCore", "RHI", "RigExecShaders" });

		// The runtime throws nothing across its API, but its headers use the
		// standard library freely.
		bEnableExceptions = true;

		// Showing a Control Rig in the level viewport is an editor feature.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "ControlRigEditor" });
		}

		// Unreal's USD build: include paths, definitions and RTTI, as its
		// own USD modules set them up.
		bUseRTTI = true;
		UnrealBuildTool.Rules.UnrealUSDWrapper.CheckAndSetupUsdSdk(Target, this);
	}
}
