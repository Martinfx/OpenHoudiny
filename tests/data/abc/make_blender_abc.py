# How the Blender-written Alembic files beside this script were made:
#
#   pip install bpy==4.5.3      (Blender 4.5 as a Python module)
#   python make_blender_abc.py
#
# shapes.abc   a static box; a cube that stands still for two frames and
#              then turns and moves; an animated camera; a Bezier curve
# deform.abc   a grid waved by a modifier (its points move every frame);
#              a grid built face by face (its faces change every frame);
#              particles (points, more each frame)
#
# Blender turns its Z-up scene to Alembic's Y-up, and writes faces wound
# the other way, as Alembic has them.
import math
import os

import bpy

here = os.path.dirname(os.path.abspath(__file__))


def fresh(frames):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = 1, frames
    scene.render.fps = 24


def shapes():
    fresh(4)
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.5))
    bpy.context.active_object.name = "box"
    bpy.ops.mesh.primitive_cube_add(size=0.5, location=(2.0, 0.0, 0.0))
    spinner = bpy.context.active_object
    spinner.name = "spinner"
    for frame, x, turn in ((1, 2.0, 0.0), (2, 2.0, 0.0), (4, 3.0, 90.0)):
        spinner.location.x = x
        spinner.rotation_euler = (0.0, 0.0, math.radians(turn))
        spinner.keyframe_insert("location", frame=frame)
        spinner.keyframe_insert("rotation_euler", frame=frame)
    bpy.ops.object.camera_add(location=(0.0, -6.0, 1.5), rotation=(math.radians(80.0), 0.0, 0.0))
    cam = bpy.context.active_object
    cam.name = "cam"
    cam.data.lens = 35.0
    for frame, x in ((1, 0.0), (4, 1.5)):
        cam.location.x = x
        cam.keyframe_insert("location", frame=frame)
    bpy.ops.curve.primitive_bezier_curve_add(location=(0.0, 0.0, 2.0))
    bpy.context.active_object.name = "path"
    bpy.ops.wm.alembic_export(filepath=os.path.join(here, "shapes.abc"), start=1, end=4, uvs=True, normals=True,
                              export_hair=False, export_particles=False)


def deform():
    fresh(3)
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=6, y_subdivisions=6, size=2.0)
    wave = bpy.context.active_object
    wave.name = "wave"
    m = wave.modifiers.new("wave", "WAVE")
    m.height = 0.3
    m.speed = 0.4
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=4, y_subdivisions=4, size=1.0, location=(3.0, 0.0, 0.0))
    built = bpy.context.active_object
    built.name = "built"
    b = built.modifiers.new("build", "BUILD")
    b.frame_start = 0.0
    b.frame_duration = 4.0
    ps = wave.modifiers.new("parts", "PARTICLE_SYSTEM").particle_system
    ps.settings.count = 9
    ps.settings.frame_start = 1
    ps.settings.frame_end = 3
    ps.settings.lifetime = 50
    bpy.ops.wm.alembic_export(filepath=os.path.join(here, "deform.abc"), start=1, end=3, uvs=False, normals=False,
                              export_hair=False, export_particles=True)


shapes()
deform()
