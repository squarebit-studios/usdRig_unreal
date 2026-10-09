// A RigExec mesh drawn on the GPU: the rig's posed points uploaded once per
// pose, normals and tangents computed by a compute shader, drawn through the
// engine's local vertex factory.
#pragma once

#include "Components/MeshComponent.h"
#include "CoreMinimal.h"

#include "RigExecSkinComponent.generated.h"

namespace UE::Geometry
{
class FDynamicMesh3;
}

/**
 * Draws one of a URigExecComponent's meshes on the GPU. Its topology, UVs,
 * colours and material slots come from the rig's Dynamic Mesh (the data the
 * rest of the plugin -- picking, Touch Pose, a subdivision surface -- reads);
 * each pose uploads only the posed positions, and the GPU recomputes the
 * normals and tangents. Created and fed by URigExecComponent; not meant to be
 * added by hand.
 *
 * Render vertices are split per (point, UV, polygon), so a per-polygon colour
 * -- a Touch Pose highlight -- keeps a hard border, while the normal is the
 * point's own and stays smooth across the splits.
 */
UCLASS(ClassGroup = (Animation), NotBlueprintable)
class RIGEXECRUNTIME_API URigExecSkinComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	URigExecSkinComponent();

	/** Takes `Mesh`'s topology, UVs and colours, and `Materials` as its slots.
	 * Rebuilds the render state. */
	void Build(const UE::Geometry::FDynamicMesh3& Mesh, const TArray<UMaterialInterface*>& Materials);
	/** Uploads `Mesh`'s current vertex positions (the same topology Build
	 * took); normals and tangents follow on the GPU. */
	void UpdatePositions(const UE::Geometry::FDynamicMesh3& Mesh);
	/** Uploads `Mesh`'s current colours. */
	void UpdateColors(const UE::Geometry::FDynamicMesh3& Mesh);

	int32 GetRenderVertexCount() const { return RenderVertex.Num(); }

	/** Write motion vectors from the pose's movement, so TSR and motion blur
	 * see the deformation (the engine's GPU-skin pass-through velocity path).
	 * Takes effect when the render state is next created. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	bool bMotionBlur = true;

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return SlotMaterials.Num(); }
	virtual UMaterialInterface* GetMaterial(int32 Index) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

	/** What the proxy is built from: static per Build, plus the current pose
	 * and colours. Shared with the render thread by copy. */
	struct FSkinData
	{
		TArray<FVector2f> UVs;
		/** Per material slot, its triangles' render vertex ids. */
		TArray<TArray<uint32>> SectionIndices;
		/** All triangles, render vertex ids (the compute shader's view). */
		TArray<uint32> Indices;
		/** Per render vertex, the triangles around its point (CSR). */
		TArray<uint32> TriOffsets;
		TArray<uint32> Tris;
	};

private:
	void GatherPositions(const UE::Geometry::FDynamicMesh3& Mesh);
	void GatherColors(const UE::Geometry::FDynamicMesh3& Mesh);

	/** The source vertex of each render vertex. */
	TArray<int32> RenderVertex;
	/** Each triangle corner's render vertex (3 per source triangle id). */
	TArray<int32> CornerRender;
	TSharedPtr<const FSkinData, ESPMode::ThreadSafe> Data;
	TArray<FVector3f> Positions;
	TArray<FColor> Colors;
	FBoxSphereBounds LocalBounds = FBoxSphereBounds(ForceInit);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> SlotMaterials;
};
