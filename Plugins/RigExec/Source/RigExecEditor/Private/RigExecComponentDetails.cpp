#include "RigExecComponentDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "RigExecComponent.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RigExecComponentDetails"

namespace
{
FDelegateHandle GFallbackHandle;

// One toast per reason per session: a level with several characters, or a
// reload, does not repeat it.
void
ToastFallback(URigExecComponent* Component, const FString& Reason)
{
	static TSet<FString> Shown;
	if (Shown.Contains(Reason))
	{
		return;
	}
	Shown.Add(Reason);
	FNotificationInfo Info(LOCTEXT("FallbackTitle", "RigExec is drawing on the CPU"));
	Info.SubText = FText::Format(LOCTEXT("FallbackBody", "{0}, so posing recomputes the meshes on the CPU and is slower. "
	                                                     "The Rig component's Details say more."),
	                             FText::FromString(Reason.Left(1).ToUpper() + Reason.Mid(1)));
	Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
	Info.ExpireDuration = 10.0f;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);
}
} // namespace

void
FRigExecComponentDetails::Register()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(URigExecComponent::StaticClass()->GetFName(),
	                                         FOnGetDetailCustomizationInstance::CreateStatic(&FRigExecComponentDetails::MakeInstance));
	GFallbackHandle = URigExecComponent::OnRenderFallback.AddStatic(&ToastFallback);
}

void
FRigExecComponentDetails::Unregister()
{
	URigExecComponent::OnRenderFallback.Remove(GFallbackHandle);
	if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		PropertyEditor->UnregisterCustomClassLayout(URigExecComponent::StaticClass()->GetFName());
	}
}

void
FRigExecComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	FString Reason;
	bool bOff = false;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		const URigExecComponent* Rig = Cast<URigExecComponent>(Object.Get());
		if (Rig && !Rig->IsDrawingOnGPU() && !Rig->GetFallbackReason().IsEmpty())
		{
			Reason = Rig->GetFallbackReason();
			bOff = !Rig->bDrawOnGPU;
			break;
		}
	}
	if (Reason.IsEmpty())
	{
		return;
	}
	// First thing in the panel: what is happening, why, and what to do.
	const FText Message =
		bOff ? LOCTEXT("OffMessage", "Drawing on the CPU: Draw On GPU is off. Turn it on (RigExec > Render) to upload each pose "
		                             "once and compute the normals on the GPU.")
		     : FText::Format(LOCTEXT("FallbackMessage", "Drawing on the CPU: {0}. Each pose recomputes the normals and re-uploads "
		                                                "the meshes on the CPU, which is slower; the GPU path runs on a "
		                                                "Shader Model 5 renderer (DirectX 12, Vulkan or Metal)."),
		                     FText::FromString(Reason));
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(TEXT("RigExecStatus"), LOCTEXT("StatusCategory", "RigExec Status"), ECategoryPriority::Important);
	Category.AddCustomRow(LOCTEXT("StatusFilter", "Render Path CPU GPU"))
		.WholeRowContent()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 8.0f, 0.0f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.WarningWithColor")))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock).Text(Message).AutoWrapText(true)
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
