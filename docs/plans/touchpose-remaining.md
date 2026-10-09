# Touch Pose: remaining work

**Status: ON HOLD** (2026-10-07). Nothing here is scheduled. Pick it back up
from this list.

## Where it stands

Done, and described in [editor-workflow.md](../editor-workflow.md#touch-pose)
and [architecture.md](../architecture.md#touch-pose):

- Regions read from the stage at load: the first `RigExecTouchRegions` layer
  per mesh, with the typed `rigExec:touch:*` properties first and the
  `touchpose:*` ones as the fallback. That gives 219 regions on the biped
  (body and both eyes).
- Ray pick on the posed skin (`FDynamicMeshAABBTree3`, rebuilt lazily after a
  pose).
- Hover, lead and selected highlights: usdview's colours at 30% opacity,
  written into the meshes' vertex colours and blended by `M_RigExecSurface`.
  They show on the GPU subdivision surface too (Squarebit Subdivs' Dynamic
  Mesh component mirrors the colours).
- Click selects; Shift toggles; Ctrl removes.
- Manipulators win: the new TRS gizmo is asked through
  `CanBeginClickDragSequence`, the legacy widget is checked, and control
  shapes are found in the hit-proxy scan.
- Touch Pose suspends while a drag is under way or within 0.35 s of the
  last pose.

**Unconfirmed:** the manipulator-precedence fix for the new TRS gizmo was
built and installed but has not been tried in the viewport. Confirm it
before anything below.

## Remaining, roughly in priority order

1. **Confirm and tune in the viewport.**
   - Gizmo precedence: drag the transform gizmo and control shapes on dense
     regions such as the mouth corners and fingers.
   - Tune the 6 px manipulator margin and the 0.35 s settle time. They are
     constants in `RigExecTouchPose.cpp`; they could become settings.
   - Decide whether camera moves with the right or middle mouse button
     should also suspend Touch Pose (today only left-button drags do).
2. **Picker integration.** Add a Touch Pose toggle button to the picker
   panel, and keep the picker and the skin in step (both already follow the
   Control Rig selection).
3. **Alt + click mirror.** Select the mirrored control's region as well, as
   the picker does. Region names follow `L_`/`R_`, or use the picker's
   mirror table.
4. **Marquee select.** Drag a rectangle over the skin and select every
   region inside it (usdview: `RegionsInBand` / `Marquee`). This must not
   fight the viewport's own marquee, so it probably needs a modifier.
5. **Layer switch.** Choose which `RigExecTouchRegions` layer is live per
   mesh (usdview: `FindLayers` with the layer row). Today the first by
   `layerOrder` always wins.
6. **Occlusion by other meshes.** A pick ignores meshes without regions, so
   clothing or props over the skin do not block it. The click path's
   hit-proxy check covers clicks but not hover. Cast against every mesh
   (cheap with lazily built trees), or use the hit proxy for hover too.
7. **Edit-mode colours.** usdview can draw every region at once in its own
   `touchpose:color` (the "edit" set) and lets you pick the lead and selected
   colours. Expose these as settings, plus the highlight opacity.
8. **Region painting.** Paint and erase faces into regions with a brush (2%
   of the bounding diagonal, as in usdview), then save back to a USD layer
   (`touchPoseModel.AssignFaces` / `EraseFaces` / `Save`). This is the
   biggest item: it needs undo, a brush cursor, and writing USD from the
   editor (`UnrealUSDWrapper`).
9. **Drag to pose.** Maya touchpose's drag-on-the-mesh posing. Needs a design
   pass for how a drag maps to the region's control (screen-space translate
   or rotate, or a tool per control type).
10. **Performance.**
    - Done: the highlight is a vertex-colour tint, and only changed regions
      are rewritten. On a subdivided character, each colour change re-runs
      Subdivs' face-varying colour interpolation (an upload, no rebuild).
    - The tree rebuild after every pose shows up when hovering while
      scrubbing slowly.
11. **Several characters and PIE.** Check hover and selection with two
    `ARigExecActor`s, and decide whether Touch Pose makes sense in PIE or at
    runtime (the runtime pieces already live in `RigExecRuntime`).
12. **Automation tests.** The picking, highlight and suspension tests run by
    hand during development (scratch scripts driving `pick_touch_region`,
    `set_touch_highlight` and `RigExec.TouchPose`) should become checked-in
    editor automation tests.

## Related, tracked separately

- **Undo "freak-out".** Reported by the user; not reproducible from script
  (single and multiple undos, IK/FK switches and keyed Sequencer edits all
  round-trip exactly). It needs the user's exact steps. It is not caused by
  Touch Pose.
