// Blueprint- and Python-callable entry points for the RigExec runtime.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "RigExecRuntimeLibrary.generated.h"

class UControlRig;

UCLASS()
class RIGEXECRUNTIME_API URigExecRuntimeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Loads a .rigexec file, poses it once per Values entry on the named
	 * channel (a property path such as "/Biped/Rig/.../M_Jaw.avars:rx"), and
	 * reports what ran: whether the file carries live property chains, each
	 * pose's evaluation time, and how far the deformed points moved from
	 * the first pose. A self-test of the runtime inside Unreal's process.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static FString RigExecSelfTest(const FString& RigExecFile, const FString& Channel,
	                               const TArray<float>& Values);

	/**
	 * Editor only: turns on the Control Rig animation mode in the level
	 * viewport, hands it this rig, and makes its controls visible -- the
	 * state the Animation Outliner's eye toggles. False outside the editor.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static bool ShowControlRigInViewport(UControlRig* ControlRig);

	/**
	 * Editor only: the Animation mode's "Controls As Overlay" setting, which
	 * draws control shapes over the meshes (most of a body rig's controls
	 * sit inside the body). Read when the mode starts, so set it before
	 * opening a sequence. False when the setting is not found.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static bool SetControlsAsOverlay(bool bOverlay);
};
