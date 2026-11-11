# Village surfaces

Run the scripts with Blender 5.2 and Unreal Editor 5.8 Python respectively.
Set `QUIETLAKE_VILLAGE_SOURCE` to the original Meshy FBX ZIP directory and optionally
set `QUIETLAKE_VILLAGE_ONLY` to comma-separated asset names.

1. `PrepareVillageAssets.py` prepares geometry, then calls `RebakeVillageAssets.py`
   to project the original albedo, tangent normal and roughness onto the final UVs.
2. `ImportVillageAssets.py` imports each FBX together with its matching maps,
   builds three LODs and configures materials and collision.

Prepared sets live in `Saved/VillageAssets`; staging and source comparisons live in
`Saved/VillageRebaked`. A set is ready only after `completed.json` is written.
`surface_pending` blocks importing a partially published set. Standalone rebaking
does not publish: use `publish_rebaked(name)` or import the staging directory with
`QUIETLAKE_VILLAGE_PREPARED`.

Do not reuse the original maps directly after decimating geometry: the surface
can stay geometrically accurate while UV interpolation visibly distorts wood.
Rebaking checks that UVs fit the texture tile, have usable area and produce a
nonempty albedo. Dense plants use smaller scaled UV gutters if fractional packing
fails. New UVs and tangent normals must always be imported together.

Base colour uses sRGB/BC7; normal maps use normal compression and invert green
for Unreal's tangent convention; roughness uses linear grayscale. Buildings use
4K colour/normal maps. LOD screen sizes are `1`, `0.12`, `0.042`.

For close visual checks run the game with `-VillagePreview -VillageTexturePreview`.
It writes fixed fence, shed, house and roof screenshots to `Saved`. Repeat with
`-ExecCmds="r.ForceLOD 1"` to inspect the first reduced LOD.
