"""Create the editable lake forest PCG graph without overwriting an existing graph."""
import unreal

path = '/Game/Forest/PCG_LakeForest'
graph = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
if graph is None:
    graph = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'PCG_LakeForest', '/Game/Forest', unreal.PCGGraph, unreal.PCGGraphFactory())
    if graph is None or not unreal.LakeForestLibrary.configure_forest_graph(graph):
        raise RuntimeError('Could not configure the lake forest graph')
    if not unreal.EditorAssetLibrary.save_loaded_asset(graph):
        raise RuntimeError('Could not save the lake forest graph')
elif not isinstance(graph, unreal.PCGGraph):
    raise RuntimeError('Forest path is occupied by a different asset type')
unreal.log('QUIET_LAKE_PCG_GRAPH_READY ' + path)
