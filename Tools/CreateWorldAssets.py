"""Create original procedural materials and import our synthesized ambience."""
import os
import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.MaterialEditingLibrary

def expression(material, cls, **props):
    node = lib.create_material_expression(material, cls)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node

def connect(source, output, target, input_name):
    # The engine silently ignores bad pin names, which once left the terrain black.
    if not lib.connect_material_expressions(source, output, target, input_name):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (source.get_name(), output, target.get_name(), input_name))

def custom(material, code, inputs):
    node = expression(material, unreal.MaterialExpressionCustom, code=code, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    entries = []
    for name, source in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        entries.append(item)
    node.set_editor_property('inputs', entries)
    for name, source in inputs:
        connect(source, '', node, name)
    return node

palette = {
    'M_Terrain': (0.15,0.24,0.08), 'M_Bark': (0.12,0.058,0.024),
    'M_Birch': (0.7,0.7,0.59), 'M_Pine': (0.035,0.13,0.065),
    'M_Leaves': (0.16,0.30,0.06), 'M_Stone': (0.25,0.28,0.25),
    'M_Grass': (0.20,0.28,0.055), 'M_Reeds': (0.28,0.27,0.065),
    'M_Planks': (0.26,0.14,0.064), 'M_Metal': (0.18,0.24,0.25),
    'M_LakeSurface': (0.018,0.13,0.15),
}
for name, rgb in palette.items():
    path = '/Game/Materials/' + name
    mat = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if not mat:
        mat = assets.create_asset(name, '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    lib.delete_all_material_expressions(mat)
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    position = expression(mat, unreal.MaterialExpressionWorldPosition)
    time = expression(mat, unreal.MaterialExpressionTime)
    color = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(*rgb,1))
    if name == 'M_Terrain':
        vertex = expression(mat, unreal.MaterialExpressionVertexColor)
        variation = custom(mat, 'return float3(1,1,1)*(0.85+0.1*sin(P.x*0.05)*sin(P.y*0.07)+0.05*sin(P.x*0.3+P.y*0.19));', [('P',position)])
        color = expression(mat, unreal.MaterialExpressionMultiply)
        connect(vertex,'',color,'A')  # VertexColor's RGB output has an empty name
        connect(variation,'',color,'B')
    else:
        detail = '0.85+0.15*sin(P.x*0.055+sin(P.y*0.033))*sin(P.z*0.07+P.y*0.05)'
        if name == 'M_Birch':
            detail = '0.22+0.78*smoothstep(0.10,0.23,abs(sin(P.z*0.12+sin(P.y*0.13))))'
        elif name in ('M_Planks','M_Bark'):
            detail = '0.7+0.3*abs(sin(P.y*0.15+sin(P.z*0.04+P.x*0.025)))'
        colored = custom(mat, 'return C*(' + detail + ');', [('P',position),('C',color)])
        color = colored
    lib.connect_material_property(color,'',unreal.MaterialProperty.MP_BASE_COLOR)
    rough = expression(mat, unreal.MaterialExpressionConstant, r=0.23 if name=='M_LakeSurface' else 0.82)
    lib.connect_material_property(rough,'',unreal.MaterialProperty.MP_ROUGHNESS)
    if name == 'M_LakeSurface':
        normal = custom(mat,'return normalize(float3(0.11*cos(P.x*0.028+P.y*0.012+T*1.1)+0.035*sin(P.y*0.11-T*1.7),0.10*sin(P.y*0.037-P.x*0.009+T*0.85),1));',[('P',position),('T',time)])
        lib.connect_material_property(normal,'',unreal.MaterialProperty.MP_NORMAL)
        metal = expression(mat, unreal.MaterialExpressionConstant,r=0.28)
        lib.connect_material_property(metal,'',unreal.MaterialProperty.MP_METALLIC)
    if name in ('M_Pine','M_Leaves','M_Grass','M_Reeds'):
        sway = custom(mat,'return float3(sin(T*1.3+P.x*0.003+P.y*0.002)*2,cos(T+P.y*0.004)*1.3,0);',[('P',position),('T',time)])
        lib.connect_material_property(sway,'',unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for name in ('A_Wind','A_Water','A_Birds'):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename',os.path.join(root,'Tools','GeneratedAudio',name+'.wav'))
    task.set_editor_property('destination_path','/Game/Audio')
    task.set_editor_property('automated',True)
    task.set_editor_property('replace_existing',True)
    task.set_editor_property('save',True)
    assets.import_asset_tasks([task])
    sound = unreal.load_asset('/Game/Audio/'+name)
    if not sound:
        raise RuntimeError('Audio import failed: '+name)
    sound.set_editor_property('looping',True)
    unreal.EditorAssetLibrary.save_asset('/Game/Audio/'+name)
unreal.log('QUIET_LAKE_WORLD_ASSETS_READY')
