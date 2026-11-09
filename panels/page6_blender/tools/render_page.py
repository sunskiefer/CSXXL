#!/usr/bin/env python3
"""CSXXL panel body render (brushed aluminium, slots, metal slider caps) with Blender's bpy.
usage: render_page.py <control_map.json> <svg> <page_index> <out_dir> [scale=2] [samples=48]
1 Blender unit = 100 px. Panel X = px x, Y = -px y (top-left origin like the control map).
Writes: <out>/body_nocaps.png  (panel, slots, grooves, no caps)
        <out>/body_caps.png    (same + caps at their stored pos)
        <out>/cap_sprite.png   (one cap, transparent)
"""
import sys, json, re, math
import bpy, bmesh
from mathutils import Vector

cmap, svgp, page_i, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
SC = float(sys.argv[5]) if len(sys.argv) > 5 else 2
SAMPLES = int(sys.argv[6]) if len(sys.argv) > 6 else 48
S = 0.01
W, H, HDR = 1280, 628, 70
CAP_W, CAP_H = 38, 46          # same cap size as the current skin
SLOT_W = 14                    # visible slot width in px

data = json.load(open(cmap))
page = [p for p in data["pages"] if p["index"] == page_i][0]
sliders = [c for c in page["controls"] if c["kind"] == "slider"]

def P(x, y, z=0):               # px -> blender
    return Vector((x * S, -y * S, z))

bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene
scn.render.engine = "CYCLES"
scn.cycles.device = "CPU"
scn.cycles.samples = SAMPLES
scn.cycles.use_denoising = True
try:
    scn.cycles.denoiser = "OPENIMAGEDENOISE"
except Exception:
    pass
scn.render.resolution_x = int(W * SC)
scn.render.resolution_y = int((H - HDR) * SC)
scn.render.resolution_percentage = 100
scn.render.image_settings.file_format = "PNG"
scn.render.image_settings.color_mode = "RGBA"
scn.view_settings.view_transform = "Standard"
scn.view_settings.look = "None"

# ---------------------------------------------------------------- materials
def mat(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    return m

def brushed_aluminium():
    m = mat("alu")
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1.2, 260.0, 1.0)      # long streaks along X
    n1 = nt.nodes.new("ShaderNodeTexNoise")
    n1.inputs["Scale"].default_value = 3.0
    n1.inputs["Detail"].default_value = 8.0
    n1.inputs["Roughness"].default_value = 0.75
    mp2 = nt.nodes.new("ShaderNodeMapping")
    mp2.inputs["Scale"].default_value = (3.0, 900.0, 1.0)     # fine hairlines
    n2 = nt.nodes.new("ShaderNodeTexNoise")
    n2.inputs["Scale"].default_value = 2.0
    n2.inputs["Detail"].default_value = 3.0
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.18
    bump.inputs["Distance"].default_value = 0.004
    mix = nt.nodes.new("ShaderNodeMath"); mix.operation = "ADD"
    rmap = nt.nodes.new("ShaderNodeMapRange")
    rmap.inputs["To Min"].default_value = 0.26
    rmap.inputs["To Max"].default_value = 0.44
    tan = nt.nodes.new("ShaderNodeTangent")
    tan.direction_type = "UV_MAP"
    bsdf.inputs["Base Color"].default_value = (0.80, 0.81, 0.83, 1)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Anisotropic"].default_value = 0.6
    bsdf.inputs["Anisotropic Rotation"].default_value = 0.0
    L = nt.links.new
    L(tc.outputs["Object"], mp.inputs["Vector"])
    L(tc.outputs["Object"], mp2.inputs["Vector"])
    L(mp.outputs["Vector"], n1.inputs["Vector"])
    L(mp2.outputs["Vector"], n2.inputs["Vector"])
    L(n1.outputs["Fac"], mix.inputs[0]); L(n2.outputs["Fac"], mix.inputs[1])
    L(mix.outputs["Value"], bump.inputs["Height"])
    L(n2.outputs["Fac"], rmap.inputs["Value"])
    L(rmap.outputs["Result"], bsdf.inputs["Roughness"])
    L(bump.outputs["Normal"], bsdf.inputs["Normal"])
    L(tan.outputs["Tangent"], bsdf.inputs["Tangent"])
    L(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return m

def simple(name, rgb, metal=0.0, rough=0.5, emit=None):
    m = mat(name)
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*rgb, 1)
    b.inputs["Metallic"].default_value = metal
    b.inputs["Roughness"].default_value = rough
    if emit:
        b.inputs["Emission Color"].default_value = (*emit, 1)
        b.inputs["Emission Strength"].default_value = 0.6
    return m

ALU = brushed_aluminium()
SLOT_BOTTOM = simple("slot_bottom", (0.01, 0.01, 0.012), 0.0, 0.9)
CAP_MET = simple("cap_graphite", (0.055, 0.055, 0.06), 1.0, 0.28)
CAP_RIDGE = simple("cap_ridge", (0.16, 0.16, 0.17), 1.0, 0.22)
CAP_INLAY = simple("cap_inlay", (0.95, 0.95, 0.93), 0.0, 0.45, emit=(0.9, 0.9, 0.88))

# ---------------------------------------------------------------- geometry helpers
def box(name, x0, y0, x1, y1, z0, z1, material, bevel=0.0, seg=2):
    cx, cy = (x0 + x1) / 2 * S, -(y0 + y1) / 2 * S
    bpy.ops.mesh.primitive_cube_add(size=1, location=(cx, cy, (z0 + z1) / 2))
    o = bpy.context.active_object
    o.name = name
    o.scale = ((x1 - x0) * S, (y1 - y0) * S, z1 - z0)
    bpy.ops.object.transform_apply(scale=True)
    if bevel:
        bm = o.modifiers.new("bev", "BEVEL")
        bm.width = bevel; bm.segments = seg; bm.limit_method = "ANGLE"
    o.data.materials.append(material)
    return o

def join(objs, name):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    objs[0].name = name
    return objs[0]

# panel body: thick slab below z=0 face, slots + grooves cut with one boolean cutter each
PAN_T = 0.30
panel = box("panel", 0, HDR, W, H, -PAN_T, 0.0, ALU)
bpy.context.view_layer.objects.active = panel
# UV for the tangent node (planar from top)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.project_from_view if False else bpy.ops.uv.cube_project(cube_size=12.8)
bpy.ops.object.mode_set(mode="OBJECT")

# slots
cut = []
for c in sliders:
    cx = c["x"] + c["w"] / 2
    cut.append(box("slot", cx - SLOT_W / 2, c["y"], cx + SLOT_W / 2, c["y"] + c["h"],
                   -0.35, 0.05, ALU, bevel=0.06, seg=3))
slotcut = join(cut, "slotcut")
for m in list(slotcut.modifiers):
    slotcut.modifiers.remove(m)
bm_ = panel.modifiers.new("slots", "BOOLEAN")
bm_.object = slotcut; bm_.operation = "DIFFERENCE"; bm_.solver = "EXACT"
slotcut.hide_render = True; slotcut.hide_viewport = True

# engraved divider lines read from the existing SVG (lines below the header)
lines = []
for m in re.finditer(r'<line x1="([\d.]+)" y1="([\d.]+)" x2="([\d.]+)" y2="([\d.]+)"', open(svgp).read()):
    x1, y1, x2, y2 = map(float, m.groups())
    if min(y1, y2) >= HDR and (x1 == x2 or y1 == y2):
        lines.append((x1, y1, x2, y2))
gcut = []
for (x1, y1, x2, y2) in lines:
    if x1 == x2:
        gcut.append(box("g", x1 - 1.0, y1, x1 + 1.0, y2, -0.045, 0.05, ALU))
    else:
        gcut.append(box("g", x1, y1 - 1.0, x2, y1 + 1.0, -0.045, 0.05, ALU))
if gcut:
    gc = join(gcut, "groovecut")
    bg = panel.modifiers.new("grooves", "BOOLEAN")
    bg.object = gc; bg.operation = "DIFFERENCE"; bg.solver = "EXACT"
    gc.hide_render = True; gc.hide_viewport = True

# bevel the cut edges on the panel
pb = panel.modifiers.new("edge", "BEVEL")
pb.width = 0.012; pb.segments = 2; pb.limit_method = "ANGLE"; pb.angle_limit = math.radians(40)

# slot bottom
slotbot = box("slotbottom", 0, HDR, W, H, -PAN_T - 0.05, -0.15, SLOT_BOTTOM)

# ---------------------------------------------------------------- cap
def make_cap(name, cx_px, top_px):
    parts = []
    x0, x1 = cx_px - CAP_W / 2, cx_px + CAP_W / 2
    y0, y1 = top_px, top_px + CAP_H
    zb, zt = 0.02, 0.30
    parts.append(box(name + "_body", x0, y0, x1, y1, zb, zt, CAP_MET, bevel=0.045, seg=3))
    # ridges: 5 above and 5 below the indicator
    mid = (y0 + y1) / 2
    for k in range(5):
        for sgn in (-1, 1):
            yy = mid + sgn * (6.5 + k * 3.2)
            parts.append(box(name + "_r", x0 + 2, yy - 0.6, x1 - 2, yy + 0.6, zt - 0.002, zt + 0.018, CAP_RIDGE, bevel=0.006, seg=1))
    parts.append(box(name + "_ind", x0 + 3, mid - 1.6, x1 - 3, mid + 1.6, zt - 0.002, zt + 0.012, CAP_INLAY, bevel=0.004, seg=1))
    return join(parts, name)

caps = []
for c in sliders:
    cx = c["x"] + c["w"] / 2
    top = c["y"] + (c["h"] - CAP_H) * (1 - c["pos"])
    caps.append(make_cap("cap_" + c["id"], cx, top))

# ---------------------------------------------------------------- camera, light, world
cam = bpy.data.cameras.new("cam"); cam.type = "ORTHO"
cam.ortho_scale = W * S
cam.clip_start = 0.01; cam.clip_end = 50
camo = bpy.data.objects.new("cam", cam)
scn.collection.objects.link(camo)
camo.location = (W * S / 2, -(HDR + H) / 2 * S, 5)
camo.rotation_euler = (0, 0, 0)
scn.camera = camo

sun = bpy.data.lights.new("sun", "SUN"); sun.energy = 1.6; sun.angle = math.radians(14)
suno = bpy.data.objects.new("sun", sun); scn.collection.objects.link(suno)
suno.rotation_euler = (math.radians(38), math.radians(-30), math.radians(25))

soft = bpy.data.lights.new("soft", "AREA"); soft.energy = 160; soft.size = 9; soft.shape = "RECTANGLE"; soft.size_y = 4
softo = bpy.data.objects.new("soft", soft); scn.collection.objects.link(softo)
softo.location = (2.0, 3.5, 5.0); softo.rotation_euler = (math.radians(-18), 0, 0)

world = bpy.data.worlds.new("w"); scn.world = world; world.use_nodes = True
wn = world.node_tree.nodes; wl = world.node_tree.links
for n in list(wn):
    wn.remove(n)
wo = wn.new("ShaderNodeOutputWorld"); bg = wn.new("ShaderNodeBackground")
wt = wn.new("ShaderNodeTexCoord"); sep = wn.new("ShaderNodeSeparateXYZ")
ramp = wn.new("ShaderNodeValToRGB")
ramp.color_ramp.elements[0].position = 0.35; ramp.color_ramp.elements[0].color = (0.75, 0.75, 0.78, 1)
ramp.color_ramp.elements[1].position = 0.85; ramp.color_ramp.elements[1].color = (1.0, 1.0, 1.02, 1)
wl.new(wt.outputs["Generated"] if False else wt.outputs["Window"], sep.inputs[0]) if False else None
mapr = wn.new("ShaderNodeMapRange")
mapr.inputs["From Min"].default_value = -1; mapr.inputs["From Max"].default_value = 1
wl.new(wt.outputs["Window"], sep.inputs[0])
wl.new(sep.outputs["X"], mapr.inputs["Value"])
wl.new(mapr.outputs["Result"], ramp.inputs["Fac"])
wl.new(ramp.outputs["Color"], bg.inputs["Color"])
bg.inputs["Strength"].default_value = 0.9
wl.new(bg.outputs["Background"], wo.inputs["Surface"])

# ---------------------------------------------------------------- renders
def render(path, show_caps=True, transparent=False):
    for c in caps:
        c.hide_render = not show_caps
    scn.render.film_transparent = transparent
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)

render(f"{out}/body_nocaps.png", show_caps=False)
render(f"{out}/body_caps.png", show_caps=True)

# single cap sprite on transparent background (4x so it stays sharp)
for o in (panel, slotbot):
    o.hide_render = True
for c in caps[1:]:
    c.hide_render = True
c0 = caps[0]
c0.hide_render = False
bb = [c0.matrix_world @ Vector(v) for v in c0.bound_box]
cx = sum(v.x for v in bb) / 8; cy = sum(v.y for v in bb) / 8
camo.location = (cx, cy, 5)
cam.ortho_scale = 48 * S
scn.render.resolution_x = 48 * 4
scn.render.resolution_y = 56 * 4
scn.render.film_transparent = True
scn.render.filepath = f"{out}/cap_sprite.png"
bpy.ops.render.render(write_still=True)
print("DONE")
