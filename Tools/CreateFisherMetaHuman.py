"""The angler MetaHuman /Game/Characters/MetaHumans/MH_Fisher: a village man of fifty-odd started from a
MetaHuman Creator preset, with a sturdy body about the mannequin's height (so the rod poses, the hand IK
and the first-person camera stay where they were), short receding hair for under the cap, stubble with
a moustache and bushy brows. The face can be tuned by hand in MetaHuman Creator afterwards.

Runs in the full editor (exports and cloud requests need it), the editor quits at the end:
    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecutePythonScript=C:/Fish/Tools/CreateFisherMetaHuman.py

Environment switches (all optional):
    QUIETLAKE_MH_PRESET=Bruce   start again from this preset (deletes the current MH_Fisher)
    QUIETLAKE_MH_CLOUD=1        request the face rig and the skin textures (Epic account sign-in)
    QUIETLAKE_MH_BUILD=1        assemble the MetaHuman into /Game/Characters/MetaHumans/Fisher
    QUIETLAKE_MH_EXPORT=1       export body and head FBX to Saved/FisherBody for GenerateFisherClothes.py
"""
import os
import unreal

PATH = "/Game/Characters/MetaHumans"
NAME = "MH_Fisher"
ASSET = "%s/%s" % (PATH, NAME)
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GROOMS = "/MetaHumanCharacter/Optional/Grooms/Bindings"

# Body measurements in cm (Fat / Masculine / Muscularity are model units, negative = masculine).
BODY = {
    "height": 180.0,
    "masculine/feminine": -1.4,
    "fat": 0.7,
    "muscularity": 0.5,
    "chest": 108.0,
    "waist": 98.0,
    "hip": 104.0,
    "across_shoulder": 39.0,
    "upper_arm_length": 35.0,
    "lower_arm_length": 26.5,
    "hand_circumference": 22.0,
    "neck": 40.0,
}
# slot -> wardrobe item
HAIR = {
    "Hair": "Hair/WI_Hair_S_RecedeMessy",
    "Beard": "Beards/WI_Beard_M_Stubble",
    "Mustache": "Mustaches/WI_Mustache_M_Stubble",
    "Eyebrows": "Eyebrows/WI_Eyebrows_M_Unruly",
    "Eyelashes": "Eyelashes/WI_Eyelashes_S_Sparse",
    "Peachfuzz": "Peachfuzz/WI_Peachfuzz_M_Thin",
}


def log(msg):
    unreal.log("QUIET_LAKE_METAHUMAN " + str(msg))


def flag(name):
    return os.environ.get(name, "") not in ("", "0")


eal = unreal.EditorAssetLibrary
sub = unreal.get_editor_subsystem(unreal.MetaHumanCharacterEditorSubsystem)
preset = os.environ.get("QUIETLAKE_MH_PRESET", "")
if preset or not eal.does_asset_exist(ASSET):
    preset = preset or "Bruce"
    if eal.does_asset_exist(ASSET):
        eal.delete_asset(ASSET)
    eal.duplicate_asset("/MetaHumanCharacter/Optional/Presets/" + preset, ASSET)
    log("from preset " + preset)
character = unreal.load_asset(ASSET)

# hair, beard and brows: the only selection in their slots; no default garment (the clothes are our own)
collection = character.internal_collection
instance = collection.default_instance
for slot, item in HAIR.items():
    wardrobe = unreal.load_asset("%s/%s" % (GROOMS, item))
    if not wardrobe:
        log("no groom " + item)
        continue
    keys = collection.get_item_keys_for_wardrobe_item(wardrobe)
    key = keys[0] if keys else collection.try_add_item_from_wardrobe_item(slot, wardrobe)
    log("groom %s %s %s" % (slot, item, instance.set_single_slot_selection(slot, key)))
log("outfit cleared %s" % instance.set_single_slot_selection("Outfits", unreal.MetaHumanPaletteItemKey()))

if not sub.try_add_object_to_edit(character):
    raise RuntimeError("MH_Fisher is open for editing elsewhere (close MetaHuman Creator)")
try:
    constraints = {str(c.name).lower().replace(" ", "_"): c for c in sub.get_body_constraints(character)}
    for key, value in BODY.items():
        c = constraints.get(key)
        if c:
            c.is_active = True
            c.target_measurement = max(c.min_measurement, min(c.max_measurement, value))
    sub.set_body_constraints(character, list(constraints.values()))
    sub.commit_body_state(character)
    log("body set")
    if flag("QUIETLAKE_MH_CLOUD"):
        rig = unreal.MetaHumanCharacterAutoRiggingRequestParams()
        rig.blocking = True
        rig.report_progress = False
        rig.rig_type = unreal.MetaHumanRigType.JOINTS_ONLY
        sub.request_auto_rigging(character, rig)
        log("rig requested")
        tex = unreal.MetaHumanCharacterTextureRequestParams()
        tex.blocking = True
        tex.report_progress = False
        sub.request_texture_sources(character, tex)
        log("textures requested")
    eal.save_asset(ASSET, only_if_is_dirty=False)
    log("can build %s" % sub.can_build_meta_human(character, True))
    if flag("QUIETLAKE_MH_BUILD"):
        build = unreal.MetaHumanCharacterEditorBuildParameters()
        build.pipeline_type = unreal.MetaHumanDefaultPipelineType.OPTIMIZED
        build.pipeline_quality = unreal.MetaHumanQualityLevel.MEDIUM
        build.absolute_build_path = PATH + "/Fisher"
        build.common_folder_path = PATH + "/Common"
        build.enable_wardrobe_item_validation = False
        try:
            sub.build_meta_human(character=character, params=build)
        except RuntimeError as e:
            # the assembly copies the shared control rigs and logs relinking errors on the way
            log("build reported: %s" % str(e).splitlines()[0][:200])
        log("built")
    if flag("QUIETLAKE_MH_EXPORT"):
        geo = unreal.MetaHumanGeometryExportParams()
        geo.project_path = PATH + "/Geometry"
        geo.body_skeletal_mesh = True
        geo.head_skeletal_mesh = True
        geo.full_body_skeletal_mesh = False
        geo.overwrite_existing_assets = True
        unreal.MetaHumanCharacterExportBlueprintLibrary.export_geometry(character, geo)
finally:
    if sub.is_object_added_for_editing(character):
        sub.remove_object_to_edit(character)
eal.save_asset(ASSET, only_if_is_dirty=False)
# the assembly and the geometry export create many packages: save them all before the editor quits
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)

if flag("QUIETLAKE_MH_EXPORT"):
    out = os.path.join(ROOT, "Saved", "FisherBody")
    os.makedirs(out, exist_ok=True)
    for part in ("Body", "Head"):
        mesh = unreal.load_asset("%s/Geometry/%s_%s" % (PATH, NAME, part))
        task = unreal.AssetExportTask()
        task.object = mesh
        task.filename = os.path.join(out, "%s_%s.fbx" % (NAME, part))
        task.automated = True
        task.replace_identical = True
        task.prompt = False
        log("export %s %s" % (part, unreal.Exporter.run_asset_export_task(task)))
log("done")
unreal.SystemLibrary.quit_editor()
