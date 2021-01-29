/* ***************************************************************** 
    MESQUITE -- The Mesh Quality Improvement Toolkit

    Copyright 2009 Sandia National Laboratories.  Developed at the
    University of Wisconsin--Madison under SNL contract number
    624796.  The U.S. Government and the University of Wisconsin
    retain certain rights to this software.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License 
    (lgpl.txt) along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA

    (2009) kraftche@cae.wisc.edu    

  ***************************************************************** */


/** \file PaverMinEdgeLengthWrapper.hpp
 *  \brief 
 *  \author Jason Kraftcheck 
 */

#ifndef MSQ_PAVER_MIN_EDGE_LENGTH_WRAPPER_HPP
#define MSQ_PAVER_MIN_EDGE_LENGTH_WRAPPER_HPP

#include "Mesquite.hpp"
#include "Mesquite_Wrapper.hpp"

namespace MESQUITE_NS {

class MESQUITE_EXPORT PaverMinEdgeLengthWrapper : public Wrapper
{
  private:
    int iterationLimit;
    int parallelIterations;
    double maxVtxMovement;
    double maxRelVtxMovement;

    void run_wrapper( MeshDomainAssoc* mesh_and_domain,
                      ParallelMesh* pmesh,
                      Settings* settings,
                      QualityAssessor* qa,
                      MsqError& err );

  public:
  
    /**
     *\param max_vertex_movement  Termination optimization if no vertex is moved
     *                            by more than this distance in the previous solver
     *                            step.
     *\param max_iterations       Termination optimizaiton after this many solver 
     *                            steps.
     */
    PaverMinEdgeLengthWrapper( 
        double max_relative_vertex_movement = 1.e-2,
        double max_vertex_movement = 1.e-3,
        int max_iterations = 50,
        int parallel_iterations = 10 )
      : iterationLimit( max_iterations ),
      parallelIterations( parallel_iterations ),
      maxVtxMovement( max_vertex_movement ),
      maxRelVtxMovement( max_relative_vertex_movement )
      {}

};


} // namespace MESQUITE_NS

#endif