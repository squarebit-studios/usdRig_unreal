"""
Places a RigExec character in the open level, ready for Sequencer.

Runs inside the Unreal Editor's Python:

    spawn_rig.py <rig.rigexec> <stage.usda> <controls.json> <Control Rig asset>

The controls file is export_controls.py's output after build_control_rig.py
has written the element names into it, and the asset is the one that script
made. Returns the RigExecActor.
"""

import sys

import unreal


def Spawn(rigexec, stage, controls, controlRig, label="RigExecCharacter"):
    asset = unreal.load_asset(controlRig)
    if asset is None:
        raise RuntimeError("no Control Rig at %s" % controlRig)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = actors.spawn_actor_from_class(unreal.RigExecActor, unreal.Vector(0, 0, 0))
    actor.set_actor_label(label)
    actor.set_control_rig_asset(asset.generated_class())
    rig = actor.get_editor_property("rig")
    for prop, path in (("rig_exec_file", rigexec), ("stage_file", stage),
                       ("controls_file", controls)):
        value = unreal.FilePath()
        value.file_path = path
        rig.set_editor_property(prop, value)
    if not rig.load():
        raise RuntimeError("the rig did not load; see LogRigExec")
    return actor


if __name__ == "__main__":
    Spawn(*sys.argv[1:5])
