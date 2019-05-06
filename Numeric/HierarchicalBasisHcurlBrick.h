// Gmsh - Copyright (C) 1997-2019 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.
//
// Contributed by Ismail Badia.
// Reference :  "Higher-Order Finite Element  Methods"; Pavel Solin, Karel
// Segeth ,
//                 Ivo Dolezel , Chapman and Hall/CRC; Edition : Har/Cdr (2003).

#ifndef HIERARCHICAL_BASIS_HCURL_BRICK_H
#define HIERARCHICAL_BASIS_HCURL_BRICK_H

#include "HierarchicalBasisHcurl.h"

/*
 * MHexahedron
 *
 *          v
 *   3----------2
 *   |\     ^   |\
 *   | \    |   | \
 *   |  \   |   |  \
 *   |   7------+---6
 *   |   |  +-- |-- | -> u
 *   0---+---\--1   |
 *    \  |    \  \  |
 *     \ |     \  \ |
 *      \|      w  \|
 *       4----------5
 *
 *  Oriented Edges:
 * e0={0, 1}, e1={0, 3}, e2={0, 4}, e3={1, 2}, e4 ={1, 5}, e5={2, 3},e6={2, 6},
 * e7={3, 7},e8={4, 5}, e9= {4, 7}, e10={5, 6}, e11={6, 7}
 *
 * Oritented Surface:
 *  s0={0, 1, 3, 2}, s1={0, 1, 4, 5}, s2={0, 3, 4, 7},
 *  s3={1, 2, 5, 5}, s4={2, 3, 6, 6}, s5={4, 5, 7, 6}
 * Local (directional) orders on mesh faces are not allowed to exceed the mini-
 * mum of the (appropriate directional) orders of approximation associated with
 * the interior of the adjacent elements. Local orders of approximation on mesh
 * edges are limited by the minimum of all (appropriate directional) orders cor-
 * responding to faces sharing that edge
 */
class HierarchicalBasisHcurlBrick : public HierarchicalBasisHcurl {
public:
  HierarchicalBasisHcurlBrick(int order);
  virtual ~HierarchicalBasisHcurlBrick();


  virtual void generateBasis(
    double const &u, double const &v, double const &w,
    std::vector<std::vector<double> > &vertexBasis,
    std::vector<std::vector<double> > &edgeBasis,
    std::vector<std::vector<double> > &faceBasis,
    std::vector<std::vector<double> > &bubbleBasis,
    std::string typeFunction) {
      if(typeFunction == "HcurlLegendre") {
        generateBasis(u, v, w, edgeBasis, faceBasis, bubbleBasis);
      }
      else if("CurlHcurlLegendre" == typeFunction) {
        generateCurlBasis(u, v, w, edgeBasis, faceBasis, bubbleBasis);
      }
      else {
        throw std::string("unknown typeFunction");
      }
    };
   virtual void orientEdge(int const &flagOrientation, int const &edgeNumber,std::vector<std::vector<double>> &edgeBasis);
  virtual void orientFace(double const &u, double const &v, double const &w,
                                                                          int const &flag1, int const &flag2, int const &flag3,
                                                                          int const &faceNumber,
                                                                          std::vector<std::vector<double>> &faceFunctions,std::string typeFunction) ;


private:
  int _pb1; // bubble function order in  direction u
  int _pb2; // bubble function order in  direction v
  int _pb3; // bubble function order in  direction w
  int _pOrderEdge[12]; // Edge functions order (pOrderEdge[0] matches the order
                       // of the edge 0)
  int _pOrderFace1[6]; // Face functions order in direction u  (pOrderFace1[0]
                       // matches the order of face 0 in direction u)
  int _pOrderFace2[6]; // Face functions order in direction v (pOrderFace[0]
                       // matches the order of face 0 in direction v)
  static double
  _affineCoordinate(const int &j, const double &u, const double &v,
                    const double &w); // affine coordinate lambdaj j=1..6

  // edgeBasis=[phie0_{0},...phie0_{pe0},phie1_{0},...phie1_{pe1}...]
  // faceBasis=[phieFf1{n1,n2} (with 0<=n1<=pf1 , 2<=n2<=pf2+1), phieFf2{n1,n2} (with 2<=n1<=pf1+1 , 0<=n2<=pf2) ]
  // bubbleBasis=[phieb1{n1,n2,n3} (with 0<=n1<=pb1 , 2<=n2<=pb2+1 , 2<=n3<=pb3+1)...]
  virtual void generateBasis(double const &u, double const &v, double const &w,
                             std::vector<std::vector<double>> &edgeBasis,
                             std::vector<std::vector<double>> &faceBasis,
                            std::vector<std::vector<double>> &bubbleBasis) ;
  virtual void generateCurlBasis(double const &u, double const &v, double const &w,
                             std::vector<std::vector<double>> &edgeBasis,
                               std::vector<std::vector<double>> &faceBasis,
                             std::vector<std::vector<double>> &bubbleBasis) ;
};
#endif
