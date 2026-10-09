// Registers the RigExec Picker tab (Window > RigExec Picker), the hover
// picker over the level viewport (Tools > RigExec > Hover Picker, or P over
// the viewport), the right-click marking menu for controls, and the TouchPose
// viewport mode (Tools > RigExec > Touch Pose).
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "RigExecComponentDetails.h"
#include "RigExecHoverPicker.h"
#include "RigExecMarkingMenu.h"
#include "RigExecTouchPose.h"
#include "SRigExecPicker.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "RigExecEditor"

static const FName GPickerTab("RigExecPicker");

static FAutoConsoleCommand GOpenPicker(TEXT("RigExec.OpenPicker"), TEXT("Opens the RigExec Picker tab."),
                                       FConsoleCommandDelegate::CreateLambda([]() {
	                                       FGlobalTabmanager::Get()->TryInvokeTab(GPickerTab);
                                       }));

static FAutoConsoleCommand GTouchPoseCommand(
	TEXT("RigExec.TouchPose"),
	TEXT("Turns TouchPose in the level viewport on (1) or off (0); toggles with no argument."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
		FRigExecTouchPose::SetEnabled(Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : !FRigExecTouchPose::IsEnabled());
	}));

static FAutoConsoleCommand GHoverPickerCommand(
	TEXT("RigExec.HoverPicker"),
	TEXT("Shows the hover picker over the level viewport (1) or hides it (0); toggles with no argument."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
		FRigExecHoverPicker::SetEnabled(Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : !FRigExecHoverPicker::IsEnabled());
	}));

class FRigExecEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()
		    ->RegisterNomadTabSpawner(GPickerTab, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
			    return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SRigExecPicker)];
		    }))
		    .SetDisplayName(LOCTEXT("PickerTab", "RigExec Picker"))
		    .SetTooltipText(LOCTEXT("PickerTip", "Select and switch a RigExec character's controls."))
		    .SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory());
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRigExecEditorModule::AddMenu));
		// The Rig component's render-path banner and the CPU-fallback toast.
		FRigExecComponentDetails::Register();
		// The hover picker's input (P) from the start, and its overlay back
		// if it was showing when the editor last closed.
		FRigExecHoverPicker::Startup();
		// The right-click marking menu for controls; installed after the
		// hover picker so it stands first in line.
		FRigExecMarkingMenu::Startup();
	}

	/** Tools > RigExec > Picker, Hover Picker and Touch Pose. */
	void AddMenu()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Tools = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Tools->FindOrAddSection("RigExec", LOCTEXT("RigExecSection", "RigExec"));
		Section.AddMenuEntry("RigExecPicker", LOCTEXT("PickerEntry", "Picker"),
		                     LOCTEXT("PickerEntryTip", "Select and switch a RigExec character's controls."),
		                     FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() {
			                     FGlobalTabmanager::Get()->TryInvokeTab(GPickerTab);
		                     })));
		Section.AddMenuEntry(
			"RigExecHoverPicker", LOCTEXT("HoverPickerEntry", "Hover Picker"),
			LOCTEXT("HoverPickerEntryTip",
			        "Draw the picker's tabs right over the level viewport (P over the viewport toggles it)."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { FRigExecHoverPicker::SetEnabled(!FRigExecHoverPicker::IsEnabled()); }),
			          FCanExecuteAction(), FIsActionChecked::CreateLambda([]() { return FRigExecHoverPicker::IsEnabled(); })),
			EUserInterfaceActionType::ToggleButton);
		Section.AddMenuEntry(
			"RigExecTouchPose", LOCTEXT("TouchPoseEntry", "Touch Pose"),
			LOCTEXT("TouchPoseEntryTip", "Pick a RigExec character's controls by touching its skin in the viewport."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { FRigExecTouchPose::SetEnabled(!FRigExecTouchPose::IsEnabled()); }),
			          FCanExecuteAction(), FIsActionChecked::CreateLambda([]() { return FRigExecTouchPose::IsEnabled(); })),
			EUserInterfaceActionType::ToggleButton);
	}

	virtual void ShutdownModule() override
	{
		FRigExecTouchPose::Shutdown();
		FRigExecHoverPicker::Shutdown();
		FRigExecMarkingMenu::Shutdown();
		FRigExecComponentDetails::Unregister();
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GPickerTab);
		}
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRigExecEditorModule, RigExecEditor)
