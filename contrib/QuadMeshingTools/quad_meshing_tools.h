// Gmsh - Copyright (C) 1997-2020 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.
//
// Author: Maxence Reberol

#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>

#include "qmt_types.hpp"
#include "qmt_projection.h"

/* QuadMeshingTools module
 * See contrib/QuadMeshingTools/README.md for a more detailed description */

namespace QMT {
  /* tempory solution to avoid initialization of gmsh api in each function that use
   * the api (risk of crash if not initialized) */
  extern bool gmsh_api_initialized;
  void initialize_gmsh_api_if_necessary();

  /**
   * @brief Fill the triangular mesh M from the gmsh mesh in the model meshName
   *
   * @warning this function calls the gmsh API
   * @param[in] meshName name of the model containing the mesh in gmsh, ignored if equals to "current"
   * @param[out] M Output mesh containing lines and triangles
   * @param[in] compute_adjacencies Fill the neighborhood datastructures
   * @param[in] add_missing_non_manifold_lines If compute_adjacencies and a non-manifold edge is not in lines, add it
   * @return True if the import succeed
   */
  bool import_TMesh_from_gmsh(const std::string& meshName, TMesh& M, 
      bool compute_adjacencies = false, bool add_missing_non_manifold_lines = false);

  /**
   * @brief Fill the quad mesh M from the gmsh mesh in the model meshName
   *
   * @warning this function calls the gmsh API
   * @param[in] meshName name of the model containing the mesh in gmsh, ignored if equals to "current"
   * @param[out] M Output mesh containing lines and qquads
   * @param[in] compute_adjacencies Fill the neighborhood datastructures
   * @return True if the import succeed
   */
  bool import_QMesh_from_gmsh(const std::string& meshName, QMesh& M, bool compute_adjacencies = true);

  /**
   * @brief Generate a quadrilateral mesh (type QMesh) from a colored triangulation. Each
   *        color (surface entity) must be a patch with four boundaries (curve entity).
   *
   * @warning this function calls the gmsh API
   * @param[in] modelName Name of the model in gmsh
   * @param[in] H_tag Index of the view that contains H (computed from cross field, used for conformal sizemap)
   * @param[in] size_min Minimum edge size in the output
   * @param[in] size_max Maximal edge size in the ouput (only used if size_min = 0.)
   * @param[in] initialGeometryName Name of the geometric model on which to project
   * @param[in] projector From projection on initial geometry, ignored if NULL
   * @param[in,out] entityToInitialEntity Entity mapping, from current entity to initial geometry. Can be completed if missing info.
   * @param[out] M Generated quad mesh
   * @return True if the generation succeed
   */
  bool generate_quad_mesh_from_gmsh_colored_triangulation(
      const std::string& modelName,
      int H_tag,
      double size_min,
      double size_max,
      QMesh& M,
      const BoundaryProjector* projector = NULL,
      const std::map<std::pair<int,int>,std::pair<int,int>>* entityToInitialEntity = NULL);

  /**
   * @brief Generate a new model (named meshName) in gmsh, which contains the quadrilateral mesh M
   *
   * @warning this function calls the gmsh API
   * @param[in] M Input quadrilateral mesh
   * @param[in] meshName name of the model to create (new model entity in gmsh)
   * @return True if the export succeed
   */
  bool export_qmesh_to_gmsh_mesh(const QMesh& M, const std::string& meshName);

  /**
   * @brief Simplify the quad mesh structure by iteratively collapsing chords.
   *
   * @param[in,out] M Mesh to simplify
   * @param[in] size_collapse Chords whose width is inferior to size_collapse will be removed
   * @param[in] nb_collapse_max Limit the number of collapses, no limit if -1
   * @param[in] projector From projection on initial geometry, ignored if NULL
   * @return True if the simplification succeed
   */
  bool simplify_quad_mesh(
      QMesh& M, 
      double size_collapse, 
      int nb_collapse_max = -1,
      const BoundaryProjector* projector = NULL);

  /**
   * @brief Smooth the vertex positions of a quad mesh, using Winslow smoothing.
   *
   * @param[in,out] M Quadrilateral mesh to smooth
   * @param[in] iter_max Number of explicit iterations (loop over all vertices)
   * @param[in] projector From projection on initial geometry, ignored if NULL
   * @warning current implementation will shrink the geometry
   * @return True if the smoothing succeed
   */
  bool smooth_quad_mesh(
      QMesh& M, 
      size_t iter_max = 10,
      const BoundaryProjector* projector = NULL);

  /**
   * WARNING: deprecated, no longer available. 
   * Can be replaced by compute_cross_field_with_multilevel_diffusion() combined
   * with create_cross_field_theta_view()
   */
  bool compute_cross_field_with_heat(
      const std::string& meshName,
      int& crossFieldTag,
      int nbSteps = 4,
      std::map<std::pair<size_t,size_t>,double>* edge_to_angle = NULL,
      int bc_expansion_layers = 0);

  bool create_cross_field_theta_view(
      const std::string& meshName,
      const std::map<std::array<size_t,2>,double>& edgeTheta,
      int& crossFieldTag);


  /****************************************************************************************/
  /******************************* prototypes, not ready **********************************/
  /****************************************************************************************/

  bool fill_vertex_sizes_from_sizemap(QMesh& M, int sizemapTag, double size_uniform = 0.);

  bool generate_quad_mesh_via_tmesh_quantization(
      const std::string& modelName,
      int sizemapTag,
      double size_min,
      double size_max,
      QMesh& M,
      const BoundaryProjector* projector = NULL,
      bool repair_decomposition = false);



  /* DataList view, only for visualization */
  bool create_scaled_cross_field_view(const std::string& meshName, int tagCrossField, int tagH, bool viewIsModelData, const std::string& viewName, int& viewTag);

  /* ModelData view, can be used for meshing */
  bool create_per_triangle_scaled_cross_field_view(const std::string& meshName, int tagCrossField, int tagSizeMap, const std::string& viewName, int& viewTag);

  bool convert_quad_mesh_to_tri_mesh(const QMesh& Q, TMesh& T);

  bool simplify_quad_mesh_by_merging_irregular_vertices(
      QMesh& M, 
      int nb_collapse_max = -1,
      const BoundaryProjector* projector = NULL);

  /****************************************************************************************/
  /* internal functions which are used in multiple QMT source files                       */
  /****************************************************************************************/
  bool compute_triangle_adjacencies(
      const std::vector<id3>& triangles,
      std::vector<sid3>& triangle_neighbors,
      std::vector<std::vector<id>>& nm_triangle_neighbors,
      std::vector<id2>& uIEdges,
      std::vector<id>& old2IEdge,
      std::vector<std::vector<id>>& uIEdgeToOld);

  bool compute_quad_adjacencies(
      const std::vector<id4>& quads,
      std::vector<sid4>& quad_neighbors,
      std::vector<std::vector<id>>& nm_quad_neighbors);

}


