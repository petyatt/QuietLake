import os,runpy,json
from pathlib import Path
source=Path('C:/Users/PT/.codex/worktrees/77d2/Fish/Saved/AssetReview/Meshy-20261101')
for species in ['Crucian','Perch','Pike']:
 os.environ['QUIETLAKE_FISH']=species
 os.environ['QUIETLAKE_MESHY_FOLDER']=str(source/species.lower())
 runpy.run_path('C:/Fish/Tools/ImportMeshyFish.py',run_name='__main__')
