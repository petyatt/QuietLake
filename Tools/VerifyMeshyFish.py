import unreal,json
sub=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
for sp in ['Crucian','Perch','Pike']:
 m=unreal.load_asset('/Game/Fish/SM_Fish_Meshy_'+sp)
 sub.set_lod_screen_sizes(m,[1.0,0.12,0.025])
 assert sub.get_lod_count(m)==3
 counts=[]
 for lod in range(3):
  section=unreal.ProceduralMeshLibrary.get_section_from_static_mesh(m,lod,0)
  counts.append(len(section[1])//3)
 assert m.get_material(0).get_name()=='M_Meshy_'+sp
 unreal.EditorAssetLibrary.save_asset(m.get_path_name())
 unreal.log('MESHY_VERIFIED '+sp+' triangles='+str(counts)+' screens='+str(sub.get_lod_screen_sizes(m))+' bounds='+str(m.get_bounds()))
