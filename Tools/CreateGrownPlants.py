import unreal

out = []
def log(s):
    out.append(str(s))

SRC = "/ProceduralVegetationEditor/SampleAssets/StarterContent/"
DST = "/Game/Plants/PV/"
copies = {
    "PV_Spruce": SRC + "ConiferTree_01/PVE_Sample_Conifer_Tree_01",
    "PV_Shrub": SRC + "Deciduous_Shrub_01/PVE_Sample_Deciduous_Shrub_01",
}

unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(["/ProceduralVegetationEditor", "/Game/Plants"], True)
for name, src in copies.items():
    dst = DST + name
    if not unreal.EditorAssetLibrary.does_asset_exist(dst):
        ok = unreal.EditorAssetLibrary.duplicate_asset(src, dst)
        log("duplicate %s -> %s : %s" % (src, dst, bool(ok)))
    asset = unreal.load_asset(dst)
    pkg = asset.get_outermost().get_name()
    # Walk every object living in this package: nodes, their settings, the export node.
    for obj in unreal.ObjectIterator(unreal.Object):
        try:
            if obj.get_outermost().get_name() != pkg:
                continue
        except Exception:
            continue
        cls = obj.get_class().get_name()
        if cls.endswith("Settings") or cls in ("PCGNode", "ProceduralVegetationGraph"):
            line = "  %s  %s" % (cls, obj.get_path_name().split(":")[-1])
            if cls == "PVExportSettings":
                p = obj.get_editor_property("export_settings")
                folder = unreal.DirectoryPath()
                folder.set_editor_property("path", DST.rstrip("/"))
                p.set_editor_property("content_browser_folder", folder)
                p.set_editor_property("mesh_name", "SKM_" + name)
                p.set_editor_property("replace_policy", unreal.PVAssetReplacePolicy.REPLACE)
                obj.set_editor_property("export_settings", p)
                p = obj.get_editor_property("export_settings")
                line += "  -> folder=%s mesh=%s type=%s wind=%s" % (
                    p.get_editor_property("content_browser_folder").path, p.get_editor_property("mesh_name"),
                    p.get_editor_property("export_mesh_type"), p.get_editor_property("wind_settings"))
            log(line)
    unreal.EditorAssetLibrary.save_asset(dst, only_if_is_dirty=False)

with open(r"C:\Fish\Saved\CreateGrownPlants.txt", "w", encoding="utf-8") as f:
    f.write("\n".join(out))
