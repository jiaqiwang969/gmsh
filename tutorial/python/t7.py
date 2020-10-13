# ------------------------------------------------------------------------------
#
#  Gmsh Python tutorial 7
#
#  Background meshes
#
# ------------------------------------------------------------------------------

import gmsh
import os
import sys

# Mesh sizes can be specified very accurately by providing a background mesh,
# i.e., a post-processing view that contains the target characteristic lengths.

gmsh.initialize()
gmsh.option.setNumber("General.Terminal", 1)

# Merge a list-based post-processing view containing the target mesh sizes:
path = os.path.dirname(os.path.abspath(__file__))
gmsh.merge(os.path.join(path, os.pardir, 't7_bgmesh.pos'))

# If the post-processing view was model-based instead of list-based (i.e. if it
# was based on an actual mesh), we would need to create a new model to contain
# the geometry so that meshing it does not destroy the background mesh. It's not
# necessary here since the view is list-based, but it does no harm:
gmsh.model.add("t7")

# Create a simple rectangular geometry:
lc = 1e-2
gmsh.model.geo.addPoint(0, 0, 0, lc, 1)
gmsh.model.geo.addPoint(.1, 0, 0, lc, 2)
gmsh.model.geo.addPoint(.1, .3, 0, lc, 3)
gmsh.model.geo.addPoint(0, .3, 0, lc, 4)
gmsh.model.geo.addLine(1, 2, 1)
gmsh.model.geo.addLine(3, 2, 2)
gmsh.model.geo.addLine(3, 4, 3)
gmsh.model.geo.addLine(4, 1, 4)
gmsh.model.geo.addCurveLoop([4, 1, -2, 3], 1)
gmsh.model.geo.addPlaneSurface([1], 1)
gmsh.model.geo.synchronize()

# Add the post-processing view as a new size field:
bg_field = gmsh.model.mesh.field.add("PostView")

# Apply the view as the current background mesh size field:
gmsh.model.mesh.field.setAsBackgroundMesh(bg_field)

# In order to compute the mesh sizes from the background mesh only, and
# disregard any other size constraints, one can set:
gmsh.option.setNumber("Mesh.CharacteristicLengthExtendFromBoundary", 0)
gmsh.option.setNumber("Mesh.CharacteristicLengthFromPoints", 0)
gmsh.option.setNumber("Mesh.CharacteristicLengthFromCurvature", 0)

# See `t10.py' for additional information: background meshes are actually a
# particular case of general "mesh size fields".

gmsh.model.mesh.generate(2)
gmsh.write("t7.msh")

# Launch the GUI to see the results:
if '-nopopup' not in sys.argv:
    gmsh.fltk.run()

gmsh.finalize()
