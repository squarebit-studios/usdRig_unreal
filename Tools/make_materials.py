"""
Makes the plugin's surface material, /RigExec/M_RigExecSurface.

Runs inside the Unreal Editor, in a project with the RigExec plugin
installed (the example project does):

    py <repo>/Tools/make_materials.py

Every USD material becomes an instance of it (URigExecComponent::Load) with
the preview surface's colour, roughness and metallic. It also blends the
vertex colour in by its alpha, which is how Touch Pose tints the skin: the
lit regions' faces carry the state colour and usdview's 30% opacity, every
other face alpha 0. The same colours reach a subdivision surface that draws
the meshes, so the tint shows either way. A little of it goes to emissive,
so a lit region still reads in shadow.

It is saved into the plugin the project loaded; copy the .uasset into
Plugins/RigExec/Content of this repository.
"""

import unreal

PATH = "/RigExec"
NAME = "M_RigExecSurface"


def _Param(material, cls, name, value, x, y):
    lib = unreal.MaterialEditingLibrary
    node = lib.create_material_expression(material, cls, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", value)
    return node


def Make():
    full = "%s/%s" % (PATH, NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())
    lib = unreal.MaterialEditingLibrary

    color = _Param(material, unreal.MaterialExpressionVectorParameter, "Color",
                   unreal.LinearColor(0.7, 0.7, 0.7, 1.0), -900, -100)
    roughness = _Param(material, unreal.MaterialExpressionScalarParameter, "Roughness", 0.6, -900, 150)
    metallic = _Param(material, unreal.MaterialExpressionScalarParameter, "Metallic", 0.0, -900, 250)
    glow = _Param(material, unreal.MaterialExpressionScalarParameter, "TouchGlow", 0.35, -900, 400)
    tint = lib.create_material_expression(material, unreal.MaterialExpressionVertexColor, -900, 50)

    # Base colour: the surface colour, tinted toward the vertex colour by its alpha.
    mix = lib.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -500, -50)
    lib.connect_material_expressions(color, "RGB", mix, "A")
    lib.connect_material_expressions(tint, "", mix, "B")
    lib.connect_material_expressions(tint, "A", mix, "Alpha")
    lib.connect_material_property(mix, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # Emissive: the tint times its alpha times TouchGlow, so a lit region reads in shadow.
    weighted = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -650, 300)
    lib.connect_material_expressions(tint, "", weighted, "A")
    lib.connect_material_expressions(tint, "A", weighted, "B")
    emissive = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -450, 300)
    lib.connect_material_expressions(weighted, "", emissive, "A")
    lib.connect_material_expressions(glow, "", emissive, "B")
    lib.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    lib.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    lib.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)

    lib.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(material, False):
        raise RuntimeError("could not save " + full)
    unreal.log("RIGEXEC_MATERIAL_OK " + full)


if __name__ == "__main__":
    Make()
