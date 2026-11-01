import os,runpy,json
from pathlib import Path
source=Path(os.environ.get('QUIETLAKE_MESHY_SOURCE','C:/Fish/Saved/AssetReview/Meshy-20261101'))
species_list=[n.strip() for n in os.environ.get('QUIETLAKE_FISH','').split(',') if n.strip()] or ['Crucian','Perch','Pike']
for species in species_list:
 os.environ['QUIETLAKE_FISH']=species
 os.environ['QUIETLAKE_MESHY_FOLDER']=str(source/species.lower())
 runpy.run_path('C:/Fish/Tools/ImportMeshyFish.py',run_name='__main__')
