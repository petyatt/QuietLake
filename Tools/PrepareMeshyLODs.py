"""Prepare review sources as static fish: texture UV0, longitudinal wave in vertex red."""
import bpy, json, math
from pathlib import Path
from mathutils import Matrix
ROOT=Path('C:/Fish')
SOURCE=Path('C:/Users/PT/.codex/worktrees/77d2/Fish/Saved/AssetReview/Meshy-20261101')
OUT=ROOT/'Tools/GeneratedFish/Meshy'
OUT.mkdir(parents=True,exist_ok=True)
report={}
for species,budgets in [('Crucian',(12500,3000,600)),('Perch',(17500,4000,750)),('Pike',(16000,4000,750))]:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(next((SOURCE/species.lower()).glob('*.fbx'))))
    obj=next(o for o in bpy.context.scene.objects if o.type=='MESH')
    bpy.ops.object.select_all(action='DESELECT');obj.select_set(True);bpy.context.view_layer.objects.active=obj
    obj.rotation_euler=(0,0,-math.pi/2)
    bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    lo=[min(v.co[k] for v in obj.data.vertices) for k in range(3)]
    hi=[max(v.co[k] for v in obj.data.vertices) for k in range(3)]
    length=hi[0]-lo[0]
    obj.data.transform(Matrix.Translation((-lo[0],-(lo[1]+hi[1])/2,-(lo[2]+hi[2])/2)))
    original=obj.data.copy();counts=[]
    for lod,budget in enumerate(budgets):
        obj.data=original.copy()
        obj.data.calc_loop_triangles();mod=obj.modifiers.new('GameBudget','DECIMATE');mod.ratio=budget/len(obj.data.loop_triangles)
        bpy.ops.object.modifier_apply(modifier=mod.name)
        for c in list(obj.data.color_attributes):obj.data.color_attributes.remove(c)
        color=obj.data.color_attributes.new(name='Swim',type='FLOAT_COLOR',domain='CORNER')
        obj.data.color_attributes.active_color_index=0
        for loop in obj.data.loops:
            u=max(0,min(1,obj.data.vertices[loop.vertex_index].co.x/length))
            color.data[loop.index].color=(u,1,1,1)
        for face in obj.data.polygons:face.use_smooth=True
        obj.data.calc_loop_triangles();count=len(obj.data.loop_triangles);counts.append(count)
        assert len(obj.data.uv_layers)>=1 and abs(count-budget)<100
        bpy.ops.export_scene.fbx(filepath=str(OUT/('SM_Fish_Meshy_%s_LOD%d.fbx'%(species,lod))),use_selection=True,object_types={'MESH'},apply_unit_scale=True,mesh_smooth_type='FACE',colors_type='LINEAR',axis_forward='-Y',axis_up='Z',bake_space_transform=True)
    report[species]={'length_cm':length*100,'triangles':counts}
    print('MESHY_PREPARED',species,report[species],flush=True)
(OUT/'manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
