# Subdivision on the GPU

A RigExec character can be drawn as a subdivision surface by **Squarebit Subdivs** (`../SquarebitSubdivs`),
with the subdivision, normals, tangents, motion vectors and the ray-tracing refit all on the GPU. Setting it
up is the same as for any other Subdivs component:

1. Select the RigExec actor.
2. **Add Component → Squarebit Subdiv Dynamic Mesh Component**.
3. Set **Subdivision Level** (1 is the usual choice).

The component gathers the rig's meshes on its own, hides them, and draws them subdivided. Touch Pose, the
picker, Sequencer and undo work as before.

Squarebit Subdivs is not part of this repository, as source or as binaries, and the example level does not
reference it. It is optional: a project that has it installed (for example, for local testing) can add the
component as above, or rebuild the example with it by setting `RIGEXEC_SETUP_SUBDIVIDE=1` before
`Setup_RigExecBiped.bat`, which then adds the component to the biped at level 1.

## Installing Subdivs into a project

```bat
build_subdivs.bat
```

This packages `..\SquarebitSubdivs` (or the folder named by `SQUAREBIT_SUBDIVS`) for this engine and installs
it into the example project. For another project, copy `Build\SqbSubdivs` into its `Plugins\SquarebitSubdivs`
folder.

RigExec does not depend on Subdivs, and the example project does not name it either. A plugin in a project's
own `Plugins` folder is enabled whenever it is there. With Subdivs installed, the biped draws subdivided.
Without it, the project opens as normal and the biped draws through RigExec's own meshes, after a load warning
about the missing component class.

Subdivs' licensing applies. The free tier builds in the interactive editor but not in an `-unattended` editor
or a commandlet. Cooked games are never gated.

## How it fits together

Neither plugin depends on the other. They meet at Unreal's own **Dynamic Mesh Component**:

```
RigExecRuntime (CPU)                           SquarebitSubdivs (GPU)
---------------------                          -----------------------------------------
Execute → posed points
  → each mesh's UDynamicMesh ────────────────► Squarebit Subdiv Dynamic Mesh Component
      vertices          (positions, every pose)    → one upload of the control points
      triangle groups   (= USD faces, once)        → subdivision stencils, normals, tangents
      UV layer          (= primvars:st, once)      → velocity, ray-tracing refit
      colour layer      (= Touch Pose tint, live)  → vertex colours (upload, no rebuild)
```

- **Topology.** RigExec puts every triangle of a USD face into one triangle group. The component walks each
  group's outline and recovers the authored quads. On the biped, 11 meshes give 35,734 faces and 35,927
  control points.
- **Welding.** Control points are the meshes' own vertices. Render vertices split only for UVs are welded back
  by vertex id, never by position, so the lips and eyelids, which touch at rest, are never fused.
- **Per pose.** While a mesh is hidden (the subdivision draws it instead), `Evaluate` writes the posed points
  and nothing else: no CPU normals and no render update of its own. The component hears the change and uploads
  the points once, in the last tick group of the frame. A mesh shown again (the component removed, or
  subdivision turned off) gets its normals back on the next tick.
- **Touch Pose.** The highlight is written into the meshes' vertex colours, and only for the regions whose
  state changed. The component mirrors colour changes onto the subdivided surface per cage face (Hard Face Colors), so every subdivided triangle takes its face's colour and a region's border stays crisp. It does this, and RigExec's surface
  material (`/RigExec/M_RigExecSurface`) blends the colour in by its alpha. The highlight therefore shows on the
  subdivided skin exactly as on the cage. Picking still runs against the posed cage. Every patch of the limit
  surface belongs to one cage face and regions are sets of cage faces, so a pick lands in the right region. It
  is only off right at a silhouette, where the cage and the limit surface part.

## Your own materials

The highlight shows only through a material that blends the vertex colour in by its alpha, as
`M_RigExecSurface` does:

```
BaseColor = lerp(Color, VertexColor.rgb, VertexColor.a)
Emissive  = VertexColor.rgb * VertexColor.a * TouchGlow
```

Untinted faces carry an alpha of 0. Set `SurfaceMaterial` on the Rig component to a material of your own that
does the same, or the highlight will not show.

## Limits

- **Picking uses the cage.** Expect a slight mismatch at silhouettes on a heavily smoothed surface.
- **Rest tangents are a perpendicular to the normal**, because the Dynamic Mesh carries no tangent frame.
  Normal maps work, but the tangent can rotate about the normal under large deformation.
- **Creases are not carried yet.** The biped has none. USD `creaseIndices`/`creaseSharpnesses` would need a
  channel on the Dynamic Mesh, or a hook in the component.
