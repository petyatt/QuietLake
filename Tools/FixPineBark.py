# The Baltic Pine bark material from Fab (Megaplant_Library) keeps four texture overrides that point
# to packages missing from the library (a developer folder and the Aleppo Pine). They load as None
# and only fill the log with LoadErrors: the two-bark master uses the Lower/Upper textures. This
# removes the empty overrides. Run again after restoring Megaplant_Library from Fab:
#   UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/FixPineBark.py
import unreal

PATH = "/Game/Megaplant_Library/Tree_Baltic_Pine/Materials/MI_Baltic_Pine_01_Bark"
mi = unreal.load_asset(PATH)
if not mi:
    unreal.log_warning("FixPineBark: %s not found" % PATH)
else:
    values = list(mi.get_editor_property("texture_parameter_values"))
    kept = [v for v in values if v.get_editor_property("parameter_value")]
    removed = [str(v.get_editor_property("parameter_info").get_editor_property("name")) for v in values if not v.get_editor_property("parameter_value")]
    if removed:
        mi.set_editor_property("texture_parameter_values", kept)
        unreal.MaterialEditingLibrary.update_material_instance(mi)
        unreal.EditorAssetLibrary.save_loaded_asset(mi, only_if_is_dirty=False)
    unreal.log("FixPineBark: removed %d empty texture overrides: %s" % (len(removed), ", ".join(removed)))
