// A RigExec rig in an Unreal scene: the binary runtime posing the rig, and
// one dynamic mesh per deformed mesh drawing what it computed.
#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "UsdWrappers/UsdStage.h"

#include "RigExecComponent.generated.h"

class UControlRigComponent;
class UMaterialInterface;
class UDynamicMeshComponent;
class URigExecSkinComponent;

namespace rigExec
{
class RigExecRuntimeReader;
}


UCLASS(ClassGroup = (Animation), meta = (BlueprintSpawnableComponent))
class RIGEXECRUNTIME_API URigExecComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	/** A component fell back to drawing on the CPU, and why (the editor shows
	 * a toast). Not fired when Draw On GPU is simply off. */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnRenderFallback, URigExecComponent*, const FString&);
	static FOnRenderFallback OnRenderFallback;

	URigExecComponent();
	virtual ~URigExecComponent() override;

	/** The rig: a poseable .rigexec (rigExecBake --poseable). Relative paths
	 * here and below are relative to the project directory. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FFilePath RigExecFile;

	/** The USD stage the rig was baked from, read once for each mesh's
	 * topology so vertex i is the rig's point i. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FFilePath StageFile;

	/** The parent of each mesh material: every USD material becomes an
	 * instance with its preview-surface colour ("Color" or "BaseColor"),
	 * "Roughness" and "Metallic". The plugin's M_RigExecSurface when empty,
	 * which also blends the vertex colour in by its alpha -- how Touch Pose
	 * tints the skin, drawn directly or through a subdivision surface. A
	 * material of your own shows Touch Pose only if it does the same. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	/** export_controls.py's description, with the element names
	 * build_control_rig.py wrote back: how each Control Rig control and
	 * channel maps onto the rig's avars. Optional. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FFilePath ControlsFile;

	/** The Control Rig whose controls pose the rig each tick. Found on the
	 * owning actor when left empty. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TObjectPtr<UControlRigComponent> ControlRig;


	/** Loads the rig and builds the meshes. False with the reason logged. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool Load();

	/** Sets one channel by property path ("/.../M_Jaw.avars:rx"); takes
	 * effect at the next Evaluate. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool SetChannel(const FString& Channel, float Value);

	/** Drops every channel set, back to the rig's baked pose. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void ClearChannels();

	/** Runs the rig and moves the meshes. Returns the evaluation time in
	 * milliseconds, negative on failure. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	float Evaluate();

	/** The meshes built by Load, in the rig's mesh order. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	TArray<UDynamicMeshComponent*> GetMeshes() const { return Meshes; }

	/** Vertex `Index` of mesh `Mesh` as the rig last placed it, in Unreal
	 * space -- for tests. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	FVector GetVertex(int32 Mesh, int32 Index) const;

	/** Reads the Control Rig's controls into avars and, when any moved,
	 * evaluates. Returns the number of avars that changed. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	int32 PullControls();

	/** The Control Rig control standing for the rig control at `Path`
	 * ("/Biped/Rig/.../L_Arm"), or None. */
	FName FindControlElement(const FString& Path) const;
	/** The Control Rig control or channel standing for the rig attribute
	 * at `AttrPath` ("/.../L_Arm.avars:ikfk"), or None. */
	FName FindChannelElement(const FString& AttrPath) const;
	/** The picker layout export_picker.py wrote beside the controls file
	 * (Biped_controls.json -> Biped_picker.json), resolved; empty if none. */
	FString PickerFilePath() const;

	/** Whether `SwitchPath` ("/.../L_Arm.avars:ikfk") is a limb's IK/FK
	 * switch this rig can match. */
	bool IsLimbSwitch(const FString& SwitchPath) const;
	/** Hands the limb switched by `SwitchPath` to its other half without
	 * moving it: the incoming half's controls are put on the limb's
	 * current joints (offsets measured at rest by export_controls.py),
	 * then the switch flips. Writes through the Control Rig with undo, so
	 * call it inside a transaction. False when there is no such limb. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool SwitchLimb(const FString& SwitchPath);
	/** The switch ("/.../L_Arm.avars:ikfk") of the limb the Control Rig
	 * control `Element` belongs to -- the switch's own control, an FK or IK
	 * control, the pole, or anything under the IK control (a foot's roll) --
	 * as usdview's ikfkMatch.LimbFor. Empty when it is on no limb. */
	FString FindLimbSwitchFor(FName Element) const;
	/** Whether the limb switched by `SwitchPath` is on its IK half now. */
	bool IsLimbIk(const FString& SwitchPath) const;
	/** The space channel on the Control Rig control `Element` (its
	 * avars:space) and the label of each space, or false when it has none
	 * or only one. */
	bool FindSpaceChannel(FName Element, FName& OutChannel, TArray<FString>& OutLabels) const;
	/** Moves `Element` to space `Index` without moving it: the space flips,
	 * the control is put back on the frame it had, and the rig evaluates.
	 * Writes through the Control Rig with undo, so call it inside a
	 * transaction. */
	bool SwitchSpace(FName Element, int32 Index);

	/** The posed frame of the rig control or joint at `Path`, in the
	 * Control Rig's space; identity when the rig has no such slot. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	FTransform GetRigFrame(const FString& Path) const;


	/** The touch regions read from the stage at load: for each mesh, the
	 * first layer of its RigExecTouchRegions, as usdview's TouchPose opens
	 * it. Regions are numbered across the meshes. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	int32 GetTouchRegionCount() const;

	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	FString GetTouchRegionName(int32 Region) const;

	/** The Control Rig control a click in `Region` selects, or None. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	FName GetTouchRegionControl(int32 Region) const;

	/** The touch region a world-space ray first hits on the posed skin, or
	 * -1 when it misses or lands on unpainted skin. `OutDistance` is the
	 * hit's distance along the ray. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	int32 PickTouchRegion(const FVector& RayOrigin, const FVector& RayDirection, double& OutDistance);

	/** The regions whose control is one of `Controls`. */
	TArray<int32> GetTouchRegionsOf(const TArray<FName>& Controls) const;

	/** Lights touch regions on the skin: `Hover` in its palette colour,
	 * `Lead` in the lead colour and `Selected` in the selected colour (-1 /
	 * empty for none), as usdview's TouchPose does. Written into the drawn
	 * meshes' vertex colours (only the regions that changed), so it rides
	 * the skin as the rig poses and reaches any renderer that draws those
	 * meshes -- a subdivision surface included. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	void SetTouchHighlight(int32 Hover, int32 Lead, const TArray<int32>& Selected);

	/** Hides the touch highlight without forgetting it while the rig is
	 * being worked (a drag, playback, an attribute edit); it returns as it
	 * was when resumed. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	void SetTouchSuspended(bool bSuspended);

	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	bool IsTouchSuspended() const;

	/** When the rig last posed (FPlatformTime::Seconds), or 0. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	double GetLastPoseTime() const { return LastPoseTime; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	/** Sends every control and channel to the rig again, re-poses, re-places
	 * the gizmos and rewrites the Touch Pose colours -- what scrubbing
	 * Sequencer does -- now and again on the next tick. Called after every
	 * editor undo or redo, which can restore state this component never saw
	 * change, before or after Sequencer re-evaluates. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void RequestResync();
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Draw the meshes on the GPU: each pose uploads the posed points once
	 * and the GPU computes the normals and tangents. Where that cannot run
	 * (no compute shaders) or with this off, the meshes draw on the CPU, which
	 * recomputes the normals and re-uploads the mesh every pose. */
	UPROPERTY(EditAnywhere, Category = "RigExec|Render")
	bool bDrawOnGPU = true;

	/** On the GPU, write motion vectors from the pose's movement, so TSR and
	 * motion blur see the character deform (the CPU fallback draws without
	 * them). */
	UPROPERTY(EditAnywhere, Category = "RigExec|Render", meta = (EditCondition = "bDrawOnGPU"))
	bool bMotionBlur = true;

	/** How the meshes are drawn now, and why when it is the CPU fallback. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "RigExec|Render")
	FString RenderPath;

	/** Whether the meshes draw on the GPU right now. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Render")
	bool IsDrawingOnGPU() const { return bGpu; }
	/** Why the meshes draw on the CPU; empty on the GPU. */
	const FString& GetFallbackReason() const { return FallbackReason; }

private:
	/** Owned; a raw pointer because the generated code cannot see the
	 * reader's definition. */
	rigExec::RigExecRuntimeReader* Reader = nullptr;
	TArray<uint8> Bytes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UDynamicMeshComponent>> Meshes;
	/** Each mesh's position in the reader's point list. */
	TArray<int32> PointSlots;
	/** The stage, kept for its up axis and units. */
	UE::FUsdStage Stage;

	struct FControlMap
	{
		FName Element;
		FString Path;
		int32 Parent = -1;
		/** A rig helper drawn as a null: placed, never read. */
		bool bNull = false;
		/** The channel fading this control (a limb's IK/FK dial): the
		 * control is shown only while the fade is above zero. -1: none. */
		int32 FadeChannel = -1;
		bool bFadeInvert = false;
		int8 Shown = -1;
		int32 Order = 0;
		double Sign[3] = {1.0, 1.0, 1.0};
		double UnitScale = 1.0;
		bool Channels[9] = {};
		/** tx ty tz rx ry rz sx sy sz as last sent: the decomposition's
		 * hint and the change test. */
		double Values[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
		double Defaults[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
	};
	struct FChannelMap
	{
		FName Element;
		FString Path;
		double Value = 0.0;
		double Default = 0.0;
		/** A dial's value names (a space switch's spaces), or empty. */
		TArray<FString> Labels;
	};
	TArray<FControlMap> ControlMaps;
	TArray<FChannelMap> ChannelMaps;
	double CentimetresPerUnit = 1.0;
	/** The asset's placement (USD row-vector), from the description. */
	double AssetToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	/** The Control Rig's root null (the description's "root"): every control
	 * keeps its USD offset and value as authored, and this one null carries
	 * the asset from USD's Y-up right-handed space into Unreal's. */
	FName UsdRoot;
	/** The USD-space root's global matrix in the Control Rig (the Y/Z swap
	 * when the hierarchy is not at hand). */
	FMatrix UsdRootGlobal() const;
	/** The global matrix of the parent of control C: its parent control or
	 * null, or the USD-space root. */
	FMatrix ParentGlobal(const class URigHierarchy* Hierarchy, int32 C) const;
	bool LoadControls();
	/** Moves every control's offset so its gizmo sits on the frame the rig
	 * computed: offset = value^-1 * frame * parentFrame^-1. */
	void PlaceControls();
	/** A control's or joint's posed frame, asset space (row vectors). */
	bool AssetFrame(const FString& Path, FMatrix& Out) const;
	/** An asset-space frame as a Control Rig global transform. */
	FMatrix ToUnreal(const FMatrix& Asset) const;
	/** The same, with the USD-space root's global already in hand (read once
	 * per placement rather than once per control). */
	FMatrix ToUnreal(const FMatrix& Asset, const FMatrix& RootGlobal) const;
	/** Puts a control on an asset-space frame (its translation only, when
	 * `bTranslateOnly`) and evaluates, so what hangs below follows. */
	bool PutControl(const FString& Path, const FMatrix& Asset, bool bTranslateOnly);
	struct FLimbMap
	{
		FString Switch;
		double IkValue = 1.0;
		TArray<FString> Joints;
		TArray<FString> FkControls;
		FString IkControl;
		FString Effector;
		FString Pole;
		/** control * joint^-1 per FK control, asset space. */
		TArray<FMatrix> FkOffsets;
		/** end joint * effector^-1, asset space. */
		FMatrix EffectorOffset = FMatrix::Identity;
		double PoleDistance = 0.0;
	};
	TArray<FLimbMap> LimbMaps;
	bool bPlaced = false;
	/** A full resync is due (RequestResync). */
	bool bResync = false;
#if WITH_EDITOR
	FDelegateHandle UndoRedoHandle;
#endif

	/** The touch regions and what picking and drawing them needs. Owned;
	 * a raw pointer for the same reason as Reader. */
	struct FRigExecTouch;
	FRigExecTouch* Touch = nullptr;
	double LastPoseTime = 0.0;
	/** Reads the regions off the open stage; after the meshes and controls. */
	void LoadTouchRegions();
	/** Writes the regions whose highlight changed into the meshes' colours. */
	void RefreshTouchColors();
	/** Whether each mesh was drawn (visible) at its last update: a hidden
	 * mesh -- another renderer draws it -- is kept to positions only, and
	 * gets its normals back when it is shown again. */
	TArray<bool> MeshDrawn;

	/** The GPU skins drawing Meshes, parallel to it; empty on the CPU path. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<URigExecSkinComponent>> Skins;
	bool bGpu = false;
	FString FallbackReason;
	/** Chooses GPU or CPU drawing for the meshes Load built. */
	void SetUpRenderPath();
	/** Whether mesh `M` is drawn by its GPU skin right now (the skin exists
	 * and nothing else -- a subdivision surface -- has hidden the mesh). */
	bool SkinDraws(int32 M) const;
};
