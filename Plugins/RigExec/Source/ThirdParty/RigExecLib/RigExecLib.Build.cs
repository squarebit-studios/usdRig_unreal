// The RigExec binary runtime, vendored from usdRig: rigExecRuntime and
// rigExecBinary are plain C++17 with no USD, prebuilt as static libraries
// (MSVC, Release, /MD). Tools/update_rigexec_lib.py refreshes them and the
// headers runtime.h reaches; SOURCE.txt names the usdRig commit.
using System.IO;
using UnrealBuildTool;

public class RigExecLib : ModuleRules
{
	public RigExecLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string Lib = Path.Combine(ModuleDirectory, "lib", "Win64");
			PublicAdditionalLibraries.Add(Path.Combine(Lib, "rigExecRuntime.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(Lib, "rigExecBinary.lib"));
		}
	}
}
