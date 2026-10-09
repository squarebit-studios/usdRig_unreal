// RigExec's GPU shaders: registers the plugin's Shaders folder as /Plugin/RigExec
// and holds the global compute shaders and the GPU skin's vertex factory. Its own module because global shaders
// must be loaded at PostConfigInit, earlier than the runtime module (which
// depends on USD and Control Rig) can load.
using UnrealBuildTool;

public class RigExecShaders : ModuleRules
{
	public RigExecShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "RenderCore", "RHI", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Projects", "Renderer" });
	}
}
