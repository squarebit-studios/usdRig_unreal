#include "RigExecActor.h"

#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "RigExecComponent.h"

ARigExecActor::ARigExecActor()
{
	// Siblings under a plain root: the Control Rig ticks after its attach
	// parent, and the rig ticks after the Control Rig.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Rig = CreateDefaultSubobject<URigExecComponent>(TEXT("Rig"));
	Rig->SetupAttachment(RootComponent);
	Controls = CreateDefaultSubobject<UControlRigComponent>(TEXT("Controls"));
	Controls->SetupAttachment(RootComponent);
	Rig->ControlRig = Controls;
	PrimaryActorTick.bCanEverTick = false;
}

void
ARigExecActor::SetControlRigAsset(UObject* Asset)
{
	if (UClass* Class = Cast<UClass>(Asset))
	{
		Controls->SetControlRigAssetReference(FControlRigAssetStrongReference(TSubclassOf<UControlRig>(Class)));
	}
	else
	{
		Controls->SetControlRigAssetReference(FControlRigAssetStrongReference(Asset));
	}
}
