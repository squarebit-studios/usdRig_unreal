#include "RigExecRuntimeLibrary.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

#include "ControlRig.h"
#include "ControlRigGizmoLibrary.h"
#if WITH_EDITOR
#include "EditMode/ControlRigEditMode.h"
#include "Editor.h"
#include "EditorModeManager.h"
#endif

// The runtime's headers are plain C++; Unreal's single-word macros must not
// reach them.
THIRD_PARTY_INCLUDES_START
#pragma push_macro("check")
#pragma push_macro("verify")
#undef check
#undef verify
#include "rigExecRuntime/runtime.h"
#pragma pop_macro("verify")
#pragma pop_macro("check")
THIRD_PARTY_INCLUDES_END

#include <cmath>
#include <string>

IMPLEMENT_MODULE(FDefaultModuleImpl, RigExecRuntime)

FString
URigExecRuntimeLibrary::RigExecSelfTest(const FString& RigExecFile, const FString& Channel,
                                        const TArray<float>& Values)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *RigExecFile))
	{
		return FString::Printf(TEXT("cannot read %s"), *RigExecFile);
	}
	std::string Error;
	const double OpenStart = FPlatformTime::Seconds();
	std::unique_ptr<rigExec::RigExecRuntimeReader> Reader =
		rigExec::RigExecRuntimeReader::Open(Bytes.GetData(), size_t(Bytes.Num()), &Error);
	const double OpenSeconds = FPlatformTime::Seconds() - OpenStart;
	if (!Reader)
	{
		return FString::Printf(TEXT("open failed: %s"), UTF8_TO_TCHAR(Error.c_str()));
	}
	const std::vector<double> Frames = Reader->GetFrameTimes();
	if (Frames.empty() || !Reader->SetFrame(Frames.front(), &Error))
	{
		return FString::Printf(TEXT("no frame: %s"), UTF8_TO_TCHAR(Error.c_str()));
	}
	FString Report = FString::Printf(
		TEXT("opened %d bytes in %.0f ms; frames %d; property chains %s\n"),
		Bytes.Num(), OpenSeconds * 1000.0, int32(Frames.size()),
		Reader->HasPropertyChains() ? TEXT("live") : TEXT("replayed"));

	std::vector<std::vector<rigExec::RrVec3f>> First;
	const std::string ChannelPath(TCHAR_TO_UTF8(*Channel));
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		Reader->ClearAvars();
		std::string Why;
		if (!Reader->SetAvar(ChannelPath, double(Values[Index]), &Why))
		{
			return Report + FString::Printf(TEXT("SetAvar refused: %s"), UTF8_TO_TCHAR(Why.c_str()));
		}
		const double Start = FPlatformTime::Seconds();
		if (!Reader->Execute(&Error))
		{
			return Report + FString::Printf(TEXT("Execute failed: %s"), UTF8_TO_TCHAR(Error.c_str()));
		}
		const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
		double Moved = 0.0;
		size_t PointCount = 0;
		const auto& Points = Reader->GetPoints();
		if (Index == 0)
		{
			for (const auto& Mesh : Points)
			{
				First.push_back(Mesh.points);
			}
		}
		for (size_t M = 0; M < Points.size() && M < First.size(); ++M)
		{
			const auto& Now = Points[M].points;
			PointCount += Now.size();
			for (size_t P = 0; P < Now.size() && P < First[M].size(); ++P)
			{
				const double DX = double(Now[P][0]) - double(First[M][P][0]);
				const double DY = double(Now[P][1]) - double(First[M][P][1]);
				const double DZ = double(Now[P][2]) - double(First[M][P][2]);
				Moved = FMath::Max(Moved, FMath::Sqrt(DX * DX + DY * DY + DZ * DZ));
			}
		}
		Report += FString::Printf(TEXT("  %s = %.3f: %.1f ms, %d points, max move %.4f\n"), *Channel,
		                          Values[Index], Milliseconds, int32(PointCount), Moved);
	}
	return Report;
}

bool
URigExecRuntimeLibrary::ShowControlRigInViewport(UControlRig* ControlRig)
{
#if WITH_EDITOR
	if (!ControlRig || !GIsEditor)
	{
		return false;
	}
	// The shape lookup only searches libraries already in memory; one not
	// yet loaded leaves every control a shape actor with no mesh.
	for (const TSoftObjectPtr<UControlRigShapeLibrary>& Library : ControlRig->GetShapeLibraries())
	{
		Library.LoadSynchronous();
	}
	FEditorModeTools& Tools = GLevelEditorModeTools();
	if (!Tools.IsModeActive(FControlRigEditMode::ModeName))
	{
		Tools.ActivateMode(FControlRigEditMode::ModeName);
	}
	FControlRigEditMode* Mode = static_cast<FControlRigEditMode*>(Tools.GetActiveMode(FControlRigEditMode::ModeName));
	if (!Mode)
	{
		return false;
	}
	Mode->AddControlRigObject(ControlRig, nullptr);
	ControlRig->SetControlsVisible(true);
	Mode->RequestToRecreateControlShapeActors(ControlRig);
	return true;
#else
	return false;
#endif
}

bool
URigExecRuntimeLibrary::SetControlsAsOverlay(bool bOverlay)
{
#if WITH_EDITOR
	// The settings class is private to the Control Rig editor: by reflection.
	UClass* Settings = FindObject<UClass>(nullptr, TEXT("/Script/ControlRigEditor.ControlRigEditModeSettings"));
	FBoolProperty* Overlay = Settings ? FindFProperty<FBoolProperty>(Settings, TEXT("bShowControlsAsOverlay")) : nullptr;
	if (!Overlay)
	{
		return false;
	}
	UObject* Defaults = Settings->GetDefaultObject();
	Overlay->SetPropertyValue_InContainer(Defaults, bOverlay);
	Defaults->SaveConfig();
	return true;
#else
	return false;
#endif
}
