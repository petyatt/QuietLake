"""Imports the angler's clothes made by Tools/GenerateFisherClothes.py as skeletal meshes:
SKM_Fisher (hands and clothes, the animated body of the character, with its own MetaHuman-shaped
skeleton SK_Fisher) and SKM_Fisher_Cap (follows the body, hidden in the first person). The UE5
mannequin skeleton is marked compatible, so the fisher animation plays the mannequin clips on it.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportFisherOutfit.py
"""
import os
import unreal

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SOURCE = os.path.join(ROOT, "Saved", "FisherBody")
DEST = "/Game/Characters/Fisher"
assets = unreal.AssetToolsHelpers.get_asset_tools()

# slot name from Blender -> material
SLOTS = {
    "Skin": "M_Fisher_Skin",
    "Jacket": "MI_Fisher_Jacket",
    "Sweater": "MI_Fisher_Sweater",
    "Trousers": "MI_Fisher_Trousers",
    "Boots": "MI_Fisher_Boots",
    "Cap": "MI_Fisher_Cap",
}


def import_skeletal(fbx, name, skeleton=None):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", os.path.join(SOURCE, fbx))
    task.set_editor_property("destination_path", DEST)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_as_skeletal", True)
    options.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_SKELETAL_MESH)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("import_animations", False)
    options.set_editor_property("create_physics_asset", False)
    if skeleton:
        options.set_editor_property("skeleton", skeleton)
    data = options.get_editor_property("skeletal_mesh_import_data")
    data.set_editor_property("import_morph_targets", False)
    data.set_editor_property("vertex_color_import_option", unreal.VertexColorImportOption.REPLACE)
    data.set_editor_property("update_skeleton_reference_pose", False)
    data.set_editor_property("use_t0_as_ref_pose", False)
    task.set_editor_property("options", options)
    assets.import_asset_tasks([task])
    mesh = unreal.load_asset("%s/%s" % (DEST, name))
    if not mesh:
        raise RuntimeError("import failed: " + fbx)
    return mesh


# the hands wear the assembled MetaHuman body skin when it exists (same UVs as the body)
MH_SKIN = "/Game/Characters/MetaHumans/Fisher/MH_Fisher/Body/Materials/MI_Body_Baked_VT"


def material_for(name):
    if name == "M_Fisher_Skin" and unreal.EditorAssetLibrary.does_asset_exist(MH_SKIN):
        return unreal.load_asset(MH_SKIN)
    return unreal.load_asset("%s/%s" % (DEST, name))


def assign_materials(mesh):
    mats = mesh.get_editor_property("materials")
    for i, slot in enumerate(mats):
        key = str(slot.get_editor_property("material_slot_name"))
        target = SLOTS.get(key.split("_")[0], None) or SLOTS.get(key)
        if target:
            slot.set_editor_property("material_interface", material_for(target))
            mats[i] = slot      # the array hands out copies: put the slot back
    mesh.set_editor_property("materials", mats)
    for slot in mesh.get_editor_property("materials"):
        mi = slot.get_editor_property("material_interface")
        unreal.log("QUIET_LAKE_FISHER_SLOT %s %s -> %s" % (mesh.get_name(), slot.get_editor_property("material_slot_name"),
                                                         mi.get_name() if mi else None))


body = import_skeletal("Fisher_Outfit.fbx", "SKM_Fisher")
skeleton = body.get_editor_property("skeleton")
cap = import_skeletal("Fisher_Cap.fbx", "SKM_Fisher_Cap", skeleton)
# distance LODs for the clothes (about 50k vertices up close)
lods = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
unreal.log("QUIET_LAKE_FISHER_LODS %s" % lods.regenerate_lod(body, 4, True, False))
for mesh in (body, cap):
    mesh.modify()
    assign_materials(mesh)
    saved = unreal.EditorAssetLibrary.save_asset(mesh.get_path_name(), only_if_is_dirty=False)
    unreal.log("QUIET_LAKE_FISHER_SAVED %s %s" % (mesh.get_name(), saved))

# The fisher animation samples the mannequin's idle, walk and jog clips on this skeleton.
manny = unreal.load_asset("/Game/Characters/Mannequins/Meshes/SK_Mannequin")
if manny:
    compatible = [c for c in skeleton.get_editor_property("compatible_skeletons") if c]
    compatible = [c for i, c in enumerate(compatible) if c.get_path_name() not in [x.get_path_name() for x in compatible[:i]]]
    if not any(c and c.get_path_name() == manny.get_path_name() for c in compatible):
        compatible.append(manny)
        skeleton.set_editor_property("compatible_skeletons", compatible)
    unreal.EditorAssetLibrary.save_loaded_asset(skeleton)
unreal.log("QUIET_LAKE_FISHER_SKELETON %s bones compatible=%s" % (skeleton.get_path_name(), skeleton.get_editor_property("compatible_skeletons")))
b = body.get_bounds()
unreal.log("QUIET_LAKE_FISHER_OUTFIT extent=(%.1f, %.1f, %.1f) origin=(%.1f, %.1f, %.1f)" % (
    b.box_extent.x, b.box_extent.y, b.box_extent.z, b.origin.x, b.origin.y, b.origin.z))
unreal.log("QUIET_LAKE_FISHER_OUTFIT_READY")
