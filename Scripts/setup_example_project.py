"""
Builds the RigExecBiped example project's content once: the Control Rig,
a level with the character, a floor and lights, and a Level Sequence
holding the character's Control Rig track. Everything is saved, so anyone
opening the project lands on a posable character.

Runs inside the Unreal Editor (as a project startup script for one launch)
with the project's own RigExecData beside it:

    RigExecData/Biped.rigexec        rigExecBake --poseable
    RigExecData/Biped_controls.json  export_controls.py

The rig's paths are stored relative to the project, and the stage is read
from the usdRig checkout beside this repository
(Examples/RigExecBiped -> ../../../usdRig/examples/biped).

The Control Rig is built in USD space (build_control_rig.py): every control
as USD authors it, with one "UsdSpace" root converting the asset.
"""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_control_rig  # noqa: E402
import spawn_rig  # noqa: E402

RIG = "RigExecData/Biped.rigexec"
CONTROLS = "RigExecData/Biped_controls.json"
STAGE = "../../../usdRig/examples/biped/Biped_stack.usda"
CONTROL_RIG = "/Game/RigExec/CR_Biped"
LEVEL = "/Game/RigExec/L_RigExec"
SEQUENCE = "/Game/RigExec/LS_RigExec"
# The biped's subdivision level for local testing with SquarebitSubdivs:
# only with RIGEXEC_SETUP_SUBDIVIDE=1 in the environment. The level this
# repository ships never references SquarebitSubdivs.
SUBDIVISION_LEVEL = 1


def _Spawn(cls, location, rotation=None):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return actors.spawn_actor_from_class(
        cls, unreal.Vector(*location), rotation or unreal.Rotator(0, 0, 0))


def AddSubdivision(actor, level=SUBDIVISION_LEVEL):
    """Adds a Squarebit Subdiv Dynamic Mesh Component to the character, as
    Add Component does in the Details panel: it gathers the rig's meshes,
    hides them and draws them subdivided on the GPU. Skipped (and logged)
    when the SquarebitSubdivs plugin is not installed."""
    cls = getattr(unreal, "SquarebitSubdivDynamicMeshComponent", None)
    if cls is None:
        unreal.log_warning("RigExec: SquarebitSubdivs is not installed; the biped draws unsubdivided")
        return None
    subobjects = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = subobjects.k2_gather_subobject_data_for_instance(actor)
    params = unreal.AddNewSubobjectParams(parent_handle=handles[0], new_class=cls, blueprint_context=None)
    handle, failure = subobjects.add_new_subobject(params)
    if not failure.is_empty():
        raise RuntimeError("could not add the subdivision component: %s" % failure)
    data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
    component = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
    component.set_editor_property("subdivision_level", level)
    unreal.log("RigExec: subdivided at level %d (%s)" % (level, component.get_name()))
    return component


def Setup():
    project = unreal.Paths.project_dir()
    controls, label = CONTROLS, "Biped"
    build_control_rig.Build(os.path.join(project, CONTROLS), CONTROL_RIG)

    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    # Rebuilding: leave the level before deleting it. The editor opens it
    # at startup, and a level deleted while open leaves the new sequence
    # bound to the old level's actor, which then resolves to nothing.
    levels.load_level("/Engine/Maps/Entry")
    for asset in (LEVEL, SEQUENCE):
        if unreal.EditorAssetLibrary.does_asset_exist(asset):
            unreal.EditorAssetLibrary.delete_asset(asset)
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        raise RuntimeError("could not remove the old %s; close it and delete "
                           "the file, then run this again" % LEVEL)
    if not levels.new_level(LEVEL):
        raise RuntimeError("could not create %s" % LEVEL)
    settings = unreal.get_editor_subsystem(
        unreal.UnrealEditorSubsystem).get_editor_world().get_world_settings()
    # Movable lights only: no precomputed lighting to build.
    settings.set_editor_property("force_no_precomputed_lighting", True)

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    # The new level comes with the template's own sky and sun; clear them so
    # the level has exactly one directional light, ours.
    template = (unreal.DirectionalLight, unreal.SkyLight, unreal.SkyAtmosphere,
                unreal.ExponentialHeightFog, unreal.VolumetricCloud,
                unreal.StaticMeshActor)
    for actor in actors.get_all_level_actors():
        if isinstance(actor, template):
            actors.destroy_actor(actor)
    floor = actors.spawn_actor_from_object(
        unreal.load_asset("/Engine/BasicShapes/Plane"), unreal.Vector(0, 0, 0))
    floor.set_actor_scale3d(unreal.Vector(20, 20, 1))
    floor.set_actor_label("Floor")
    sun = _Spawn(unreal.DirectionalLight, (0, 0, 300),
                 unreal.Rotator(roll=0, pitch=-35, yaw=-125))
    sun.set_actor_label("Sun")
    sky = _Spawn(unreal.SkyLight, (0, 0, 300))
    sky.set_actor_label("SkyLight")
    # Movable lights: nothing to bake, so a fresh clone opens without a
    # lighting build.
    # Through the property, so the setting is saved with the level.
    for actor in (sun, sky, floor):
        for component in actor.get_components_by_class(unreal.SceneComponent):
            component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    _Spawn(unreal.SkyAtmosphere, (0, 0, 0)).set_actor_label("SkyAtmosphere")

    unreal.log("RigExec: level actors %s" % sorted(
        a.get_class().get_name() for a in actors.get_all_level_actors()))

    character = spawn_rig.Spawn(RIG, STAGE, controls, CONTROL_RIG, label=label)
    if os.environ.get("RIGEXEC_SETUP_SUBDIVIDE", "") == "1":
        AddSubdivision(character)

    # The level is saved BEFORE the sequence binds to the character: a
    # binding made while the level only exists in memory does not resolve
    # once the project is reopened.
    if not levels.save_current_level():
        raise RuntimeError("could not save %s" % LEVEL)
    folder, name = SEQUENCE.rsplit("/", 1)
    sequence = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    sequence.set_playback_start(0)
    sequence.set_playback_end(120)
    binding = sequence.add_possessable(character)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    unreal.LevelSequenceEditorBlueprintLibrary.open_level_sequence(sequence)
    unreal.ControlRigSequencerLibrary.find_or_create_control_rig_component_track(
        world, sequence, binding)
    player = _Spawn(unreal.LevelSequenceActor, (0, -200, 0))
    player.set_actor_label(label + "Sequence")
    player.set_sequence(sequence)

    unreal.EditorAssetLibrary.save_loaded_asset(sequence, False)
    if not levels.save_current_level():
        raise RuntimeError("could not save %s" % LEVEL)
    unreal.SystemLibrary.execute_console_command(world, "DumpUnbuiltLightInteractions")
    unreal.log("RigExec: example project ready (%s, %s)" % (LEVEL, SEQUENCE))


_state = {"wait": 150, "busy": False}


def _Tick(delta):
    if _state["busy"]:
        return
    _state["wait"] -= 1
    if _state["wait"] > 0:
        return
    unreal.unregister_slate_post_tick_callback(_handle)
    _state["busy"] = True
    try:
        Setup()
        unreal.log("RIGEXEC_SETUP_OK")
    except Exception:
        import traceback
        unreal.log_error("RIGEXEC_SETUP_FAILED " + traceback.format_exc())
    _state["busy"] = False
    unreal.SystemLibrary.quit_editor()


_handle = unreal.register_slate_post_tick_callback(_Tick)
