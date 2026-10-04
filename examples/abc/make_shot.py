# How shot.abc was made -- a set and a camera from Blender, as a layout
# department would hand them over:
#
#   pip install bpy==4.5.3      (Blender 4.5 as a Python module)
#   python examples/abc/make_shot.py
#
# A corner of a ruin -- two walls, a column, two crates -- and a camera
# that dollies in on it over two seconds, 24 frames a second. Blender
# turns its Z-up scene to Alembic's Y-up.
import math
import os

import bpy
from mathutils import Vector

here = os.path.dirname(os.path.abspath(__file__))

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.frame_start, scene.frame_end = 1, 48
scene.render.fps = 24


def box(name, size, at, turn=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=at, rotation=(0.0, 0.0, math.radians(turn)))
    o = bpy.context.active_object
    o.name = name
    o.data.name = name
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return o


box("back_wall", (3.2, 0.24, 1.7), (0.0, 1.3, 0.85))
box("side_wall", (0.24, 2.2, 1.2), (-1.7, 0.3, 0.6))
bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=0.17, depth=1.9, location=(1.15, 0.7, 0.95))
bpy.context.active_object.name = "column"
box("crate_a", (0.5, 0.5, 0.5), (0.75, -0.45, 0.25), 20.0)
box("crate_b", (0.36, 0.36, 0.36), (-0.95, -0.55, 0.18), -12.0)

# The camera: from the right, coming in, always on the middle of the set.
bpy.ops.object.camera_add()
cam = bpy.context.active_object
cam.name = "cam"
cam.data.lens = 32.0
target = Vector((0.0, 0.2, 0.55))
for frame, at in ((1, (2.9, -4.6, 1.25)), (48, (1.5, -3.3, 1.0))):
    cam.location = at
    cam.rotation_euler = (target - Vector(at)).to_track_quat("-Z", "Y").to_euler()
    cam.keyframe_insert("location", frame=frame)
    cam.keyframe_insert("rotation_euler", frame=frame)

bpy.ops.wm.alembic_export(filepath=os.path.join(here, "shot.abc"), start=1, end=48, uvs=True, normals=True,
                          export_hair=False, export_particles=False)
