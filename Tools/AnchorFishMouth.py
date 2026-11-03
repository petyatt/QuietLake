import unreal
lib=unreal.MaterialEditingLibrary
for sp in ['Crucian','Perch','Pike']:
 mat=unreal.load_asset('/Game/Fish/M_Meshy_'+sp)
 transform=lib.get_material_property_input_node(mat,unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
 wave=lib.get_inputs_for_material_expression(mat,transform)[0]
 code=wave.get_editor_property('code')
 assert 'float u' in code
 code=code.replace('0.12 + pow(u, 1.7)','pow(u, 1.7)').replace('C * Len * d * d','C * Len * (d * d - 0.16)')
 wave.set_editor_property('code',code)
 lib.recompile_material(mat)
 unreal.EditorAssetLibrary.save_asset(mat.get_path_name())
 unreal.log('MOUTH_ANCHORED '+sp)
