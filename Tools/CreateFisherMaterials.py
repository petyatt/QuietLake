"""Materials for the angler's clothes (Tools/GenerateFisherClothes.py): one cloth master M_Fisher_Cloth and
an instance per garment, plus M_Fisher_Skin for the hands until the MetaHuman skin is assembled.

The clothes carry UV0 in metres (round the body and up, along the sleeves) and vertex colour R = wear.
The master reads: tileable weave normal (canvas or knit), grunge blotches for colour variation,
worn colour and roughness where the vertex colour says, stitched quilting channels every 8 cm and a
buttoned placket down the front (jacket only).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/CreateFisherMaterials.py
"""
import os
import unreal

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEST = "/Game/Characters/Fisher"
TEX = DEST + "/Textures"
lib = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


# --- textures ----------------------------------------------------------------------------------
def import_texture(name, normal):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", os.path.join(ROOT, "Tools", "GeneratedFisher", name + ".png"))
    task.set_editor_property("destination_path", TEX)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    tools.import_asset_tasks([task])
    tex = unreal.load_asset("%s/%s" % (TEX, name))
    tex.set_editor_property("srgb", False)
    tex.set_editor_property("compression_settings",
                            unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT)
    eal.save_loaded_asset(tex)
    return tex


grunge = import_texture("T_Fisher_Grunge", False)
canvas = import_texture("T_Fisher_Canvas_N", True)
knit = import_texture("T_Fisher_Knit_N", True)


# --- graph helpers -----------------------------------------------------------------------------
class Graph:
    def __init__(self, mat):
        self.mat = mat
        self.y = 0

    def node(self, cls, **props):
        self.y += 60
        n = lib.create_material_expression(self.mat, cls, -800, self.y)
        for k, v in props.items():
            n.set_editor_property(k, v)
        return n

    def val(self, x):
        if isinstance(x, (int, float)):
            return (self.node(unreal.MaterialExpressionConstant, r=float(x)), "")
        if isinstance(x, tuple):
            return x
        return (x, "")

    def op(self, cls, *inputs, names=("A", "B")):
        n = self.node(cls)
        for name, x in zip(names, inputs):
            src, out = self.val(x)
            lib.connect_material_expressions(src, out, n, name)
        return n

    def mul(self, a, b): return self.op(unreal.MaterialExpressionMultiply, a, b)
    def add(self, a, b): return self.op(unreal.MaterialExpressionAdd, a, b)
    def sub(self, a, b): return self.op(unreal.MaterialExpressionSubtract, a, b)
    def div(self, a, b): return self.op(unreal.MaterialExpressionDivide, a, b)
    def lerp(self, a, b, t): return self.op(unreal.MaterialExpressionLinearInterpolate, a, b, t, names=("A", "B", "Alpha"))
    def one(self, cls, a): return self.op(cls, a, names=("",))
    def sat(self, a): return self.one(unreal.MaterialExpressionSaturate, a)
    def frac(self, a): return self.one(unreal.MaterialExpressionFrac, a)
    def abs(self, a): return self.one(unreal.MaterialExpressionAbs, a)

    def mask(self, a, r=False, g=False, b=False):
        n = self.node(unreal.MaterialExpressionComponentMask, r=r, g=g, b=b, a=False)
        src, out = self.val(a)
        lib.connect_material_expressions(src, out, n, "")
        return n

    def scalar(self, name, default):
        return self.node(unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=default)

    def vector(self, name, rgb):
        return self.node(unreal.MaterialExpressionVectorParameter, parameter_name=name,
                         default_value=unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))

    def texture(self, name, tex, sampler, uv):
        n = self.node(unreal.MaterialExpressionTextureSampleParameter2D, parameter_name=name, texture=tex, sampler_type=sampler)
        src, out = self.val(uv)
        lib.connect_material_expressions(src, out, n, "UVs")
        return n


def new_material(name):
    path = "%s/%s" % (DEST, name)
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    mat = tools.create_asset(name, DEST, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("used_with_skeletal_mesh", True)
    return mat


# --- cloth master ------------------------------------------------------------------------------
mat = new_material("M_Fisher_Cloth")
g = Graph(mat)
uv = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
u = g.mask(uv, r=True)
v = g.mask(uv, g=True)
dirt = g.texture("Grunge", grunge, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, g.mul(uv, g.scalar("GrungeScale", 1.2)))
weave = g.texture("Weave", canvas, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, g.mul(uv, g.scalar("WeaveScale", 30.0)))
vc = g.node(unreal.MaterialExpressionVertexColor)

# wear: the vertex colour, broken up by the middle blotches
wear = g.sat(g.mul((vc, "R"), g.add(0.55, g.mul((dirt, "G"), 0.9))))
base = g.mul(g.vector("BaseColor", (0.085, 0.085, 0.06)), g.lerp(0.8, 1.15, (dirt, "R")))
base = g.mul(base, g.lerp(0.9, 1.06, (dirt, "B")))
colour = g.lerp(base, g.vector("WornColor", (0.16, 0.15, 0.11)), wear)

# quilting: a stitched groove in the middle of every 8 cm channel, dashed stitches along it
f = g.frac(g.div(u, 0.08))
d = g.abs(g.sub(f, 0.5))
groove = g.sat(g.sub(1.0, g.mul(d, 22.0)))
stitch = g.sat(g.mul(g.sub(g.frac(g.div(v, 0.011)), 0.4), 6.0))
quilt = g.mul(g.scalar("Quilt", 0.0), g.add(g.mul(groove, 0.35), g.mul(g.mul(groove, groove), g.mul(stitch, 0.25))))
colour = g.mul(colour, g.sub(1.0, quilt))

# placket and buttons down the front (U = 0 is the front centre), between the hem and the collar
placket = g.scalar("Placket", 0.0)
seam = g.sat(g.sub(1.0, g.mul(g.abs(u), 160.0)))
band = g.mul(g.sat(g.mul(g.sub(v, 0.86), 80.0)), g.sat(g.mul(g.sub(1.42, v), 80.0)))
dv = g.mul(g.sub(g.frac(g.div(v, 0.115)), 0.5), 0.115)
dist = g.op(unreal.MaterialExpressionDistance, g.op(unreal.MaterialExpressionAppendVector, u, dv), g.node(unreal.MaterialExpressionConstant2Vector, r=0.0, g=0.0))
button = g.mul(g.mul(g.sat(g.mul(g.sub(0.0085, dist), 600.0)), band), placket)
colour = g.mul(colour, g.sub(1.0, g.mul(g.mul(seam, placket), 0.35)))
colour = g.lerp(colour, g.vector("ButtonColor", (0.012, 0.011, 0.010)), button)
lib.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)

rough = g.lerp(g.scalar("Roughness", 0.85), g.scalar("WornRoughness", 0.95), wear)
rough = g.add(rough, g.mul(g.sub((dirt, "B"), 0.5), 0.12))
rough = g.lerp(rough, 0.4, button)
lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
lib.connect_material_property(g.node(unreal.MaterialExpressionConstant, r=0.35), "", unreal.MaterialProperty.MP_SPECULAR)
normal = g.lerp(g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0, 0, 1, 1)), (weave, "RGB"),
                g.scalar("WeaveStrength", 0.5))
lib.connect_material_property(normal, "", unreal.MaterialProperty.MP_NORMAL)
lib.layout_material_expressions(mat)
lib.recompile_material(mat)
eal.save_loaded_asset(mat)
unreal.log("FISHER_MATERIAL M_Fisher_Cloth")

# --- hands (until the MetaHuman skin is assembled) -------------------------------------------------
skin = new_material("M_Fisher_Skin")
s = Graph(skin)
lib.connect_material_property(s.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.36, 0.23, 0.16, 1)), "", unreal.MaterialProperty.MP_BASE_COLOR)
lib.connect_material_property(s.node(unreal.MaterialExpressionConstant, r=0.55), "", unreal.MaterialProperty.MP_ROUGHNESS)
lib.connect_material_property(s.node(unreal.MaterialExpressionConstant, r=0.4), "", unreal.MaterialProperty.MP_SPECULAR)
lib.recompile_material(skin)
eal.save_loaded_asset(skin)

# --- one instance per garment -----------------------------------------------------------------------
GARMENTS = {
    # name: base colour, worn colour, roughness, worn roughness, weave texture, weave scale, weave strength, grunge scale, quilt, placket
    "MI_Fisher_Jacket": ((0.048, 0.050, 0.036), (0.11, 0.10, 0.075), 0.85, 0.95, canvas, 30.0, 0.5, 1.2, 1.0, 1.0),
    "MI_Fisher_Trousers": ((0.032, 0.038, 0.050), (0.09, 0.088, 0.078), 0.85, 0.95, canvas, 34.0, 0.5, 1.5, 0.0, 0.0),
    "MI_Fisher_Sweater": ((0.075, 0.062, 0.050), (0.11, 0.095, 0.08), 0.95, 0.97, knit, 16.0, 0.8, 2.0, 0.0, 0.0),
    "MI_Fisher_Boots": ((0.016, 0.020, 0.017), (0.075, 0.055, 0.035), 0.42, 0.92, canvas, 10.0, 0.08, 3.0, 0.0, 0.0),
    "MI_Fisher_Cap": ((0.048, 0.045, 0.040), (0.08, 0.075, 0.065), 0.92, 0.95, knit, 40.0, 0.35, 4.0, 0.0, 0.0),
}
for name, (bc, wc, r, wr, tex, scale, strength, gscale, quilt_on, placket_on) in GARMENTS.items():
    path = "%s/%s" % (DEST, name)
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    mi = tools.create_asset(name, DEST, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    mi.set_editor_property("parent", mat)
    lib.set_material_instance_vector_parameter_value(mi, "BaseColor", unreal.LinearColor(*bc, 1.0))
    lib.set_material_instance_vector_parameter_value(mi, "WornColor", unreal.LinearColor(*wc, 1.0))
    lib.set_material_instance_scalar_parameter_value(mi, "Roughness", r)
    lib.set_material_instance_scalar_parameter_value(mi, "WornRoughness", wr)
    lib.set_material_instance_texture_parameter_value(mi, "Weave", tex)
    lib.set_material_instance_scalar_parameter_value(mi, "WeaveScale", scale)
    lib.set_material_instance_scalar_parameter_value(mi, "WeaveStrength", strength)
    lib.set_material_instance_scalar_parameter_value(mi, "GrungeScale", gscale)
    lib.set_material_instance_scalar_parameter_value(mi, "Quilt", quilt_on)
    lib.set_material_instance_scalar_parameter_value(mi, "Placket", placket_on)
    eal.save_loaded_asset(mi)
    unreal.log("FISHER_MATERIAL " + name)

# the old flat-colour materials of the box outfit
for old in ("M_Fisher_Boots", "M_Fisher_Pants", "M_Fisher_Sweater", "M_Fisher_Jacket", "M_Fisher_Hat"):
    if eal.does_asset_exist("%s/%s" % (DEST, old)):
        eal.delete_asset("%s/%s" % (DEST, old))
unreal.log("FISHER_MATERIALS_READY")
