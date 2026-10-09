// The Rig component's Details: a banner at the top when its meshes draw on the
// CPU instead of the GPU, saying why in a sentence.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FRigExecComponentDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FRigExecComponentDetails>(); }
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

	/** Registers the layout and the CPU-fallback toast; at module startup. */
	static void Register();
	static void Unregister();
};
