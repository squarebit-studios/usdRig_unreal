// A RigExec character ready for Sequencer: the rig and its meshes, and a
// Control Rig whose controls pose them.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "RigExecActor.generated.h"

class UControlRigComponent;
class URigExecComponent;

UCLASS()
class RIGEXECRUNTIME_API ARigExecActor : public AActor
{
	GENERATED_BODY()

public:
	ARigExecActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RigExec")
	TObjectPtr<URigExecComponent> Rig;

	/** Set its Control Rig Class to the asset build_control_rig.py made. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RigExec")
	TObjectPtr<UControlRigComponent> Controls;

	/** Points Controls at a Control Rig asset (or its class). */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void SetControlRigAsset(UObject* Asset);
};
