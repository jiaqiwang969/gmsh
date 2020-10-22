// Gmsh - Copyright (C) 1997-2020 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#include <map>
#include <iostream>
#include "meshQuadQuasiStructured.h"
#include "meshGFace.h"
#include "GmshMessage.h"
#include "GFace.h"
#include "GModel.h"
#include "MVertex.h"
#include "MTriangle.h"
#include "MQuadrangle.h"
#include "MLine.h"
#include "GmshConfig.h"
#include "Context.h"
#include "Options.h"
#include "fastScaledCrossField.h"
#include "meshQuadPatterns.h"

#include "meshRefine.h"
#include "Generator.h"
#include "PView.h"
#include "PViewOptions.h"
#include "Field.h"
#include "geolog.h"
#include "meshWinslow2d.h"
#include "gmsh.h"
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include "qmt_utils.hpp" // For debug printing

#if defined(_OPENMP)
#include <omp.h>
#endif

using std::vector;
using std::array;
using std::unordered_map;
using std::unordered_set;


namespace QSQ {
  constexpr bool DBG_VERBOSE = false;
  constexpr bool DBG_VIZU = false;

  constexpr bool PARANO = false;

  using vec3 = std::array<double,3>;

  template<class T>
    std::ostream& operator<<(std::ostream& os, const std::vector<T>& values) { 
      os << "[";
      for (size_t i = 0; i < values.size(); ++i) {
        os << values[i];
        if (i != values.size() - 1) {
          os << ", ";
        }
      }
      os << "]";
      return os;
    }

  template<class T> 
    void sort_unique(std::vector<T>& vec) {
      std::sort( vec.begin(), vec.end() );
      vec.erase( std::unique( vec.begin(), vec.end() ), vec.end() );
    }

  template<class T1, class T2> 
  T2 sort_unique_with_perm(
        const std::vector<T1>& in, 
        std::vector<T1>& uniques,
        std::vector<T2>& old2new) {

      std::vector<T2> ids(in.size());
      for(T2 k = 0; k != in.size(); ++k) ids[k]=k;

      std::sort(ids.begin(), ids.end(), 
          [&in](const T2& a, const T2&b){ return (in[a] < in[b]); }
          );

      uniques.resize(in.size());
      old2new.resize(in.size());
      for(T2 k = 0; k != in.size(); ++k) uniques[k]=in[k];

      std::sort(uniques.begin(), uniques.end());
      uniques.erase( std::unique(uniques.begin(), uniques.end()), 
          uniques.end());
      T2 ic = 0; // indice current
      T2 ir = 0; // indice representant
      T2 cur_rep = 0; // indice of current representant
      while(ic < in.size()){
        ic = ir;
        while(ic < in.size() && in[ids[ic]]==in[ids[ir]]){
          old2new[ids[ic]] = cur_rep;
          ++ic;
        }
        ir = ic;
        ++cur_rep;
      }
      return (T2) uniques.size();
  }

  template<class T> 
    void append(std::vector<T>& v1, const std::vector<T>& v2) {
      v1.insert(v1.end(),v2.begin(),v2.end());
    }

  template<class T> 
    std::vector<T> intersection(const std::vector<T>& v1, const std::vector<T>& v2) {
      std::vector<T> s1 = v1;
      std::vector<T> s2 = v2;
      sort_unique(s1);
      sort_unique(s2);
      std::vector<T> s3;
      set_intersection(s1.begin(),s1.end(),s2.begin(),s2.end(), std::back_inserter(s3));
      return s3;
    }

  template<class T> 
    std::vector<T> difference(const std::vector<T>& v1, const std::vector<T>& v2) {
      std::vector<T> s1 = v1;
      std::vector<T> s2 = v2;
      sort_unique(s1);
      sort_unique(s2);
      std::vector<T> s3;
      set_difference(s1.begin(),s1.end(),s2.begin(),s2.end(), std::inserter(s3,s3.begin()));
      return s3;
    }


  std::array<int,4> rotateCanonical(std::array<int,4> indices) {
    int minVal = std::numeric_limits<int>::max();
    size_t minValLv = 0;
    for (size_t lv = 0; lv < 4; ++lv) {
      if (indices[lv] < minVal) {
        minVal = indices[lv];
        minValLv = lv;
      }
    }
    std::rotate(indices.begin(), indices.begin() + minValLv, indices.end());
    return indices;
  }


  std::vector<GFace*> model_faces(GModel* gm) {
    std::vector<GFace*> faces;
    for(GModel::fiter it = gm->firstFace(); it != gm->lastFace(); ++it) {
      faces.push_back(*it);
    }
    return faces;
  }

  std::vector<GEdge*> model_edges(GModel* gm) {
    std::vector<GEdge*> edges;
    std::vector<GFace*> faces = model_faces(gm);
    for (GFace* gf: faces) {
      for (GEdge* ge: gf->edges()) {
        edges.push_back(ge);
      }
      for (GEdge* ge: gf->embeddedEdges()) {
        edges.push_back(ge);

      }
    }
    sort_unique(edges);
    return edges;
  }


  inline void normalize_accurate(SVector3& a) {
    double amp = std::abs(a.data()[0]);
    amp = std::max(amp,std::abs(a.data()[1]));
    amp = std::max(amp,std::abs(a.data()[2]));
    if (amp == 0.) {
      Msg::Error("cannot normalize vector whose length is strictly 0 !");
      return ;
    }
    a = amp * a;
    a.normalize();
  }

  inline double clamp(double x, double lower, double upper) { return std::min(upper, std::max(x, lower)); }

  inline double angleVectors(SVector3 a, SVector3 b) {
    if (a.normSq() == 0. || b.normSq() == 0.) return DBL_MAX;
    normalize_accurate(a);
    normalize_accurate(b);
    return acos(clamp(dot(a,b),-1.,1.)); 
  }

  template <class ITERATOR> 
    bool buildBoundary (ITERATOR beg, ITERATOR end, vector<MVertex*>& bnd){
      std::vector<MEdge> eds,veds;

      for (ITERATOR ite = beg; ite != end;++ite){
        for (size_t j=0;j<(size_t)(*ite)->getNumEdges();j++){
          eds.push_back((*ite)->getEdge(j));
        }
      }
      MEdgeLessThan melt;
      std::sort(eds.begin(),eds.end(), melt);
      for(size_t i=0;i<eds.size();i++){
        if (i != eds.size()-1 && eds[i] == eds[i+1])i++;
        else veds.push_back(eds[i]);
      }

      std::vector<std::vector<MVertex *> > vsorted;
      bool oks = SortEdgeConsecutive(veds, vsorted);
      if (!oks) {
        // DBG("issue with", veds.size());
        // for (auto edge: veds) {
        //   DBG(edge.getMinVertex()->getNum(),edge.getMaxVertex()->getNum());
        // }
        // DBG("-----");
        return false;
      }
      if (vsorted.empty()){
        return false;
      }
      else if (vsorted.size() > 1){
        printf("ARGHTTT %lu\n",vsorted.size());
        return false;
      }

      /* Reverse vertices if necessary, to keep coherent with elements orientation */
      {
        MEdge e = veds[0];
        MVertex* v1 = e.getVertex(0);
        MVertex* v2 = e.getVertex(1);
        auto it = std::find(vsorted[0].begin(),vsorted[0].end(),v1);
        if (it == vsorted[0].end()) {
          Msg::Error("buildBoundary(): vertex not found in sorted vertices, weird");
          return false;
        }
        size_t i = it - vsorted[0].begin();
        size_t i_next = (i+1)%vsorted[0].size();
        size_t i_prev = (i-1+vsorted[0].size())%vsorted[0].size();
        if (vsorted[0][i_next] == v2) { 
          // good ordering
        } else if (vsorted[0][i_prev] == v2) { // apply reverse
          std::reverse(vsorted[0].begin(),vsorted[0].end());
        } else {
          Msg::Error("buildBoundary(): second vertex not found in adjacent sorted vertices, weird");
          return false;
        }
      }
      bnd = vsorted[0];
      return true;
    }

  bool verticesStrictlyInsideCavity(GFace* gf, const std::vector<MElement*>& quads,
      const std::vector<MVertex*>& bnd, std::vector<MVertex*>& inside) {
    std::vector<MVertex*> vert;
    vert.reserve(4*quads.size());
    for (MElement* f: quads) for (size_t lv = 0; lv < 4; ++lv) {
      MVertex* v = f->getVertex(lv);
      vert.push_back(v);
    }
    sort_unique(vert);
    inside = difference(vert,bnd);
    return true;
  }

  int surfaceEulerCharacteristicDiscrete(GFace* gf) {
    if (gf->triangles.size() == 0) {
      Msg::Error("no triangulation for face %i, cannot compute discrete Euler characteristic", gf->tag());
      return std::numeric_limits<int>::max();
    }
    vector<size_t> vertices;
    vector<array<size_t,2> > edges;
    vertices.reserve(3*gf->triangles.size());
    edges.reserve(3*gf->triangles.size());
    for (MTriangle* t: gf->triangles) {
      for (size_t lv = 0; lv < 3; ++lv) {
        size_t v1 = t->getVertex(lv)->getNum();
        size_t v2 = t->getVertex((lv+1)%3)->getNum();
        array<size_t,2> vPair = {v1,v2};
        if (v1 > v2) vPair = {v2,v1};
        edges.push_back(vPair);
        vertices.push_back(v1);
      }
    }
    sort_unique(vertices);
    sort_unique(edges);
    int S = gf->triangles.size();
    int E = edges.size();
    int V = vertices.size();
    return V - E + S;
  }


  struct GFaceInfo {
    int chi = 0;
    std::map<GVertex*,int> cornerAngle;
    std::set<GVertex*> cornerIsNonManifold;
    std::array<std::set<GVertex*>,5> bdrValVertices;
    int intSumVal3mVal5 = 0;
  };

  bool fillGFaceInfo(GFace* gf, GFaceInfo& info) {
    info.chi = 0;
    info.cornerAngle.clear();
    info.cornerIsNonManifold.clear();
    for (auto& a: info.bdrValVertices) a.clear();
    info.intSumVal3mVal5 = 0;
    
    if (gf->triangles.size() == 0) {
      if (gf->quadrangles.size() != 0) {
        Msg::Error("fillGFaceInfo: there are %li quads in face %li, cannot compute angles from triangles", 
            gf->quadrangles.size(),gf->tag());
        return false;
      }
      Msg::Debug("compute trianguled mesh to fill GFaceInfo ...");
      int algo = gf->getMeshingAlgo();
      gf->setMeshingAlgo(ALGO_2D_FRONTAL);
      meshGFace mesher;
      mesher(gf);
      if (gf->triangles.size() == 0) {
        Msg::Error("failed to compute triangulated mesh for surface with tag %i", gf->tag());
        return -1;
      }
      gf->setMeshingAlgo(algo);
    }
    std::unordered_map<GVertex*,vector<MElement*> > corner2tris;
    std::unordered_map<GVertex*,double> corner2angle;
    for (MTriangle* t: gf->triangles) {
      for (size_t lv = 0; lv < 3; ++lv) {
        MVertex* v = t->getVertex(lv);
        GVertex* gv = v->onWhat()->cast2Vertex();
        if (gv != nullptr) {
          MVertex* vPrev = t->getVertex((3+lv-1)%3);
          MVertex* vNext = t->getVertex((lv+1)%3);
          SVector3 pNext = vNext->point();
          SVector3 pPrev = vPrev->point();
          SVector3 pCurr = v->point();
          double agl = angleVectors(pNext-pCurr,pPrev-pCurr);
          corner2tris[gv].push_back(t);
          corner2angle[gv] += agl;
        }
      }
    }
    /* Check if corner is manifold */
    for (const auto& kv: corner2tris) {
      GVertex* gv = kv.first;
      std::vector<MElement*> elts = kv.second;
      vector<MVertex*> bnd;
      bool okb = buildBoundary(elts.begin(),elts.end(), bnd);
      if (!okb) {
        info.cornerIsNonManifold.insert(gv);
        continue;
      }
      double angle = corner2angle[gv];
      double angle_deg = 180. / M_PI * angle;
      if (angle_deg < 90. + 45.) {
        info.bdrValVertices[1].insert(gv);
      } else if (angle_deg < 180. + 45.) {
        info.bdrValVertices[2].insert(gv);
      } else if (angle_deg < 270. + 45.) {
        info.bdrValVertices[3].insert(gv);
      } else if (angle_deg < 360.) {
        info.bdrValVertices[4].insert(gv);
      } else {
        Msg::Error("weird angle, corner (surf=%i,node=%i), angle = %f deg", gf->tag(), gv->tag(), angle_deg);
        continue;
      }
    }
    info.chi = surfaceEulerCharacteristicDiscrete(gf);

    /* discrete topological relations between irregular vertices:
     *  sum3m5 = n_val3 - n_val5 = 4 \chi + m_val3 - m_val1  + 2 m_val4 */
    info.intSumVal3mVal5 = 4*info.chi + int(info.bdrValVertices[3].size()) 
      - int(info.bdrValVertices[1].size()) + 2 * int(info.bdrValVertices[4].size());

    return true;
  }

  constexpr size_t NO_ID = std::numeric_limits<size_t>::max();

  /************************************/
  /***** Half edge data structure *****/
  /************************************/
  struct HalfEdge {
    /* Indices of other half-edges */
    size_t prev;
    size_t next;
    size_t opposite;
    /* Indices of other mesh entities */
    size_t vertex; /* the one at the tip of the arrow */
    size_t face;
  };

  struct Vertex {
    size_t he; /* reference to one half edge */
    SVector3 p; /* coordinates */
    bool isSingularity; /* singularities are irregular vertices to be preserved */
    MVertex* ptr;
    // size_t num; /* unique global identifier in gmsh */
  };

  struct Face {
    size_t he; /* reference to one half edge of the face */
    MElement* ptr;
    // size_t num; /* unique global identifier in gmsh */
  };

  struct MeshHalfEdges {
    std::vector<Vertex> vertices;
    std::vector<HalfEdge> hedges;
    std::vector<Face> faces;

    inline size_t next(size_t he) const {return hedges[he].next; }
    inline size_t opposite(size_t he) const {return hedges[he].opposite; }
    inline size_t prev(size_t he) const {return hedges[he].prev; }
    inline size_t vertex(size_t he, size_t lv) const {
      return (lv == 0) ? hedges[prev(he)].vertex : hedges[he].vertex;
    }

    std::vector<size_t> face_vertices(size_t f) const {
      size_t he = faces[f].he;
      vector<size_t> vert;
      do {
        vert.push_back(hedges[he].vertex);
        he = hedges[he].next;
      } while (he != faces[f].he);
      return vert;
    }

    size_t face_vertices(size_t f, std::vector<size_t>& vert) const {
      vert.clear();
      size_t he = faces[f].he;
      do {
        vert.push_back(hedges[he].vertex);
        he = hedges[he].next;
      } while (he != faces[f].he);
      return vert.size();
    }

    int vertexFaceValence(size_t v, bool& onBoundary) const {
      onBoundary = false;
      int valence = 0;
      size_t he_bdr = NO_ID;
      size_t he = vertices[v].he;
      if (he == NO_ID) return 0;
      do { /* turn around vertex v */
        size_t cand = opposite(next(he));
        if (cand == NO_ID) {
          he_bdr = next(he);
          break;
        }
        he = cand;
        valence += 1;
      } while (he != vertices[v].he);

      if (he_bdr == NO_ID) return valence;

      /* Boundary case, unroll from he_bdr */
      onBoundary = true;
      valence = 0;
      he = he_bdr;
      do { /* turn around vertex v */
        he = opposite(next(he));
        valence += 1;
      } while (he != vertices[v].he && he != NO_ID);
      return valence;
    }

    int vertexFaces(size_t v, std::vector<size_t>& faces) const {
      faces.clear();
      int valence = 0;
      size_t he_bdr = NO_ID;
      size_t he = vertices[v].he;
      do { /* turn around vertex v */
        size_t cand = opposite(next(he));
        if (cand == NO_ID) {
          he_bdr = next(he);
          break;
        }
        he = cand;
        faces.push_back(hedges[he].face);
        valence += 1;
      } while (he != vertices[v].he);

      if (he_bdr == NO_ID) return valence;

      /* Boundary case, unroll from he_bdr */
      faces.clear();
      valence = 0;
      he = he_bdr;
      do { /* turn around vertex v */
        faces.push_back(hedges[he].face);
        he = opposite(prev(he));
        valence += 1;
      } while (he != vertices[v].he && he != NO_ID);
      return valence;
    }

    /* Warning: for faster performance, but enough memory must be avalaible ! */
    int vertexFaces(size_t v, size_t faces[]) const {
      int valence = 0;
      size_t he_bdr = NO_ID;
      size_t he = vertices[v].he;
      do { /* turn around vertex v */
        size_t cand = opposite(next(he));
        if (cand == NO_ID) {
          he_bdr = next(he);
          break;
        }
        he = cand;
        faces[valence] = hedges[he].face;
        valence += 1;
      } while (he != vertices[v].he);

      if (he_bdr == NO_ID) return valence;

      /* Boundary case, unroll from he_bdr */
      valence = 0;
      he = he_bdr;
      do { /* turn around vertex v */
        faces[valence] = hedges[he].face;
        he = opposite(prev(he));
        valence += 1;
      } while (he != vertices[v].he && he != NO_ID);
      return valence;
    }

    int vertexHalfEdges(size_t v, std::vector<size_t>& hes) const {
      hes.clear();
      size_t he_bdr = NO_ID;
      size_t he = vertices[v].he;
      do { /* turn around vertex v */
        hes.push_back(he);
        size_t cand = opposite(next(he));
        if (cand == NO_ID) {
          he_bdr = next(he);
          break;
        }
        he = cand;
      } while (he != vertices[v].he);
      if (he_bdr == NO_ID) return hes.size();

      /* Boundary case, unroll from he_bdr */
      hes.clear();
      he = he_bdr;
      do { /* turn around vertex v */
        hes.push_back(he);
        he = opposite(prev(he));
      } while (he != vertices[v].he && he != NO_ID);
      return hes.size();
    }


    int faceAdjacentFaces(size_t f, std::vector<size_t>& afaces) const { 
      afaces.clear();
      size_t he = faces[f].he;
      do {
        if (opposite(he) != NO_ID) {
          afaces.push_back(hedges[opposite(he)].face);
        }
        he = hedges[he].next;
      } while (he != faces[f].he);
      return (int) afaces.size();
    }

    bool vertexIsRegular(size_t v) {
      bool onBdr;
      int val = vertexFaceValence(v,onBdr);
      return (onBdr && val == 2) || (!onBdr && val == 4);
    };

  };
  /************************************/
  /************************************/

  using si2 = std::array<size_t,2>;

  struct si2hash {
    size_t operator()(std::array<size_t,2> p) const noexcept {
      return size_t(p[0]) << 32 | p[1];
    }
  };

  inline std::array<double,3> convert(const SVector3& vec) {
    return {vec.data()[0],vec.data()[1],vec.data()[2]};
  }

  inline std::vector<std::array<double,3> > convert(const std::vector<SVector3>& vecs) {
    std::vector<std::array<double,3> > vecs2(vecs.size());
    for (size_t i = 0; i < vecs2.size(); ++i) vecs2[i] = convert(vecs[i]);
    return vecs2;
  }

  void geolog_halfedge(const MeshHalfEdges& M, size_t he, double value, const std::string& viewName) {
    size_t v1 = M.hedges[M.hedges[he].prev].vertex;
    size_t v2 = M.hedges[he].vertex;
    SVector3 p1 = M.vertices[v1].p;
    SVector3 p2 = M.vertices[v2].p;
    /* line */
    vector<vec3> pts = {p1,p2};
    GeoLog::add(pts, value , viewName);
  }

  void geolog_face(const MeshHalfEdges& M, size_t f, double value, const std::string& viewName) {
    vector<size_t> vert = M.face_vertices(f);
    vector<vec3> pts(vert.size());
    for (size_t lv = 0; lv < pts.size(); ++lv) {
      pts[lv] = M.vertices[vert[lv]].p;
    }
    vector<double> values(vert.size(),1.);
    GeoLog::add(pts,values,viewName);
  }

  int createMeshHalfEdges(GFace* gf, MeshHalfEdges& M, const std::vector<MVertex*>& singularVertices) {
    /* warning: does not scale on big big model because of numToHVertex */
    M.vertices.clear();
    M.hedges.clear();
    M.faces.clear();

    unordered_map<MVertex*,size_t> mv2new;
    M.vertices.reserve(4*gf->quadrangles.size());
    M.hedges.reserve(4*gf->quadrangles.size());
    M.faces.reserve(gf->quadrangles.size());

    for (MQuadrangle* q: gf->quadrangles) {
      /* Create vertices if necessary */
      size_t quad[4];
      for (size_t le = 0; le < 4; ++le) {
        MVertex* v1 = q->getVertex(le);
        size_t nv = NO_ID;
        auto it = mv2new.find(v1);
        if (it != mv2new.end()) nv = it->second;
        if (nv == NO_ID) {
          nv = M.vertices.size();
          M.vertices.resize(nv+1);
          mv2new[v1] = nv;
          M.vertices[nv].p = v1->point();
          M.vertices[nv].he = NO_ID;
          M.vertices[nv].ptr = v1;
          // M.vertices[nv].num = num;
          M.vertices[nv].isSingularity = false;
        }
        quad[le] = nv;
      }
      /* Create half-edges */
      size_t faceNo = M.faces.size();
      size_t he0 = M.hedges.size();
      M.hedges.resize(he0+4);
      for (size_t k = 0; k < 4; ++k) {
        M.hedges[he0+k].face = faceNo;
        M.hedges[he0+k].opposite = NO_ID; /* later */
        M.hedges[he0+k].vertex = quad[(1+k)%4];
        if (M.vertices[quad[(1+k)%4]].he == NO_ID) {
          M.vertices[quad[(1+k)%4]].he = he0+k;
        }
        /* next */
        if (k != 3) {
          M.hedges[he0+k].next = he0+k+1;
        } else {
          M.hedges[he0+k].next = he0;
        }
        /* prev */
        if (k != 0) {
          M.hedges[he0+k].prev = he0+k-1;
        } else {
          M.hedges[he0+k].prev = he0+3;
        }
      }
      /* Create face */
      M.faces.resize(faceNo+1);
      M.faces[faceNo].he = he0;
      // M.faces[faceNo].num = q->getNum();
      M.faces[faceNo].ptr = q;
    }

    /* Connectivity */
    vector<array<size_t,2> > vPairs(M.hedges.size());
    for (size_t i = 0; i < M.hedges.size(); ++i) {
      size_t v1 = M.hedges[M.hedges[i].prev].vertex;
      size_t v2 = M.hedges[i].vertex;
      array<size_t,2> vPair = {v1,v2};
      if (v1 > v2) vPair = {v2,v1};
      vPairs[i] = vPair;
    }
    vector<size_t> old2new;
    vector<array<size_t,2> > uniques;
    sort_unique_with_perm(vPairs, uniques, old2new);
    vector<array<size_t,2>> new2old(uniques.size(),{NO_ID,NO_ID});
    for (size_t i = 0; i < M.hedges.size(); ++i) {
      size_t ni = old2new[i];
      if (ni >= new2old.size()) {
        Msg::Error("redirection: ni=%li is superior to new2old size %li", ni, new2old.size());
        return -1;
      }
      if (new2old[ni][0] == NO_ID) {
        new2old[ni][0] = i;
      } else if (new2old[ni][1] == NO_ID) {
        new2old[ni][1] = i;
      } else {
        Msg::Error("non manifold quad mesh (face with tag %i), cannot build half edge datastructure", gf->tag());
        return -1;
      }
    }
    for (size_t i = 0; i < uniques.size(); ++i) {
      size_t h1 = new2old[i][0];
      size_t h2 = new2old[i][1];
      if (h1 != NO_ID && h2 != NO_ID) {
        M.hedges[h1].opposite = h2;
        M.hedges[h2].opposite = h1;
      }
    }

    /* Flag singular vertices */
    for (MVertex* mv: singularVertices) {
      auto it = mv2new.find(mv);
      if (it == mv2new.end()) {
        Msg::Error("singular vertex with num=%i not found in half edges vertices, vertex not in GFace quads ?", mv->getNum());
        continue;
      }
      M.vertices[it->second].isSingularity = true;
    }

    return 0;
  }

  bool boundaryHalfEdgesFromQuads(const MeshHalfEdges& M, const std::unordered_set<size_t>& quads, 
      std::vector<size_t>& boundary) {
    std::unordered_set<size_t> hes;
    for (size_t f: quads) {
      size_t he = M.faces[f].he;
      do {
        hes.insert(he);
        he = M.next(he);
      } while (he != M.faces[f].he);
    }

    std::unordered_set<size_t>::iterator it_he = hes.begin();
    while (it_he != hes.end()) {
      size_t he = *it_he;
      size_t he_op = M.opposite(he);
      auto it_op = hes.find(he_op);
      if (it_op != hes.end()) {
        hes.erase(it_op);
        it_he = hes.erase(it_he);
      } else {
        it_he++;
      }
    }

    boundary.reserve(hes.size());
    for (size_t he: hes) boundary.push_back(he);

    return true;
  }

  void removeInteriorHalfEdges(const MeshHalfEdges& M, std::vector<size_t>& hes) {
    std::vector<size_t>::iterator it_he = hes.begin();
    while (it_he != hes.end()) {
      size_t he = *it_he;
      size_t he_op = M.opposite(he);
      auto it_op = std::find(hes.begin(),hes.end(), he_op);
      if (it_op != hes.end()) {
        hes.erase(it_op);
        it_he = hes.erase(it_he);
      } else {
        it_he++;
      }
    }
  }

  bool orderedHalfEdgesFromStack(const MeshHalfEdges& M, 
      const std::vector<size_t>& hes_stack, 
      std::vector<size_t>& orderedHes) {
    orderedHes.clear();
    if (hes_stack.size() < 3) {
      Msg::Error("orderedHalfEdgesFromStack: not enough half edges: %li", hes_stack.size());
      return false;
    }
    orderedHes.reserve(hes_stack.size());

    /* Order boundary half edges in sides */
    for (size_t he0: hes_stack) {
      size_t he = he0;
      do {
        size_t v2 = M.vertex(he,1);
        /* Add current half edge to current side */
        orderedHes.push_back(he);

        /* Find next half edge */
        bool found = false;
        for (size_t he2: hes_stack) if (he2 != he && M.vertex(he2,0) == v2) {
          he = he2;
          found = true;
          break;
        }
        if (!found) {
          return false;
        }
      } while (he != he0);
      break;
    }
    return true;
  }

  inline int valenceInsideQuads(const MeshHalfEdges& M, const std::unordered_set<size_t>& quads, size_t v) {
    constexpr size_t BSIZE = 24;
    size_t faces[BSIZE];
    int val = M.vertexFaces(v, faces);
    if ((size_t) val >= BSIZE) {
      Msg::Error("valence is too high (%i) compared to buffer size %li, memory corrupted, abort", val, BSIZE);
      GeoLog::add(M.vertices[v].p, double(val), "val"+std::to_string(val));
      GeoLog::flush();
      gmsh::fltk::run();
      abort();
    }
    int count = 0;
    for (size_t i = 0; i < (size_t) val; ++i) {
      if (quads.find(faces[i]) != quads.end()) count += 1;
    }
    return count;
  }

  inline int valenceOutsideQuads(const MeshHalfEdges& M, const std::unordered_set<size_t>& quads, size_t v) {
    constexpr size_t BSIZE = 24;
    size_t faces[BSIZE];
    int val = M.vertexFaces(v, faces);
    if ((size_t) val >= BSIZE) {
      Msg::Error("valence is too high (%i) compared to buffer size %li, memory corrupted, abort", val, BSIZE);
      abort();
    }
    int count = 0;
    for (size_t i = 0; i < (size_t) val; ++i) {
      if (quads.find(faces[i]) == quads.end()) count += 1;
    }
    return count;
  }

  struct FlipInfo {
    size_t he = NO_ID;
    size_t nq = NO_ID;
    std::array<size_t,4> nvs = {NO_ID,NO_ID,NO_ID,NO_ID};
  };

  struct FCavity {
    /* Data */
    MeshHalfEdges& M;
    std::vector<size_t> hes; /* ordered half edges */
    std::vector<uint8_t> side; /* side associated to each half-edge */
    std::unordered_set<size_t> quads; /* quads inside, unordered_set for queries */


    /* Methods */
    FCavity(MeshHalfEdges& M_) : M(M_) { }

    FCavity & operator= ( const FCavity & other) {
      hes = other.hes;
      side = other.side;
      quads = other.quads;
      return *this;
    }

    bool init(const std::vector<size_t>& quadsInit) {
      if (quadsInit.size() < 1) {
        Msg::Error("FCavity init: expecting at least 1 quad, not %li", quadsInit.size());
        return false;
      }
      /* Add quads and collect bdr half edges */
      vector<size_t> hes_stack;
      for (size_t f: quadsInit) {
        quads.insert(f);
        size_t he = M.faces[f].he;
        do {
          hes_stack.push_back(he);
          he = M.next(he);
        } while (he != M.faces[f].he);
      }
      removeInteriorHalfEdges(M, hes_stack);

      bool oks = orderedHalfEdgesFromStack(this->M, hes_stack, this->hes);
      if (!oks) {
        Msg::Error("failed to determine sides from %li boundary half edges (%li quads)", hes_stack.size(), quads.size());
        return false;
      }
      int nsides = updateSides();
      if (nsides <= 0) {
        Msg::Error("should have at least one side, not %i", nsides);
        return false;
      }

      return true;
    }

    bool growByFlip(size_t i, FlipInfo& info, bool rejectNewSings = true) { /* i is index of half edge in hes */
      if (DBG_VERBOSE) {DBG("growByFlip ...", i, hes.size());}
      if (i >= hes.size()) {
        if (DBG_VERBOSE) {DBG("can't flip because", i, hes.size());}
        info.nq = NO_ID;
        return false;
      }
      const size_t he0_op = hes[i];
      const size_t he0 = M.opposite(he0_op);
      if (he0 == NO_ID) {
        if (DBG_VERBOSE) {DBG("can't flip because", i, hes.size(), he0_op, he0);}
        info.nq = NO_ID;
        return false; /* half-edge on bdr */
      }
      info.he = he0_op;
      info.nq = M.hedges[he0].face;
      const size_t he1 = M.hedges[he0].next;
      const size_t he2 = M.hedges[he1].next;
      const size_t he3 = M.hedges[he2].next;
      const size_t he1_op = M.hedges[he1].opposite;
      const size_t he2_op = M.hedges[he2].opposite;
      const size_t he3_op = M.hedges[he3].opposite;
      // const size_t q0 = M.hedges[he0_op].face; /* initial quad inside cavity */
      const size_t q1 = (he1_op != NO_ID) ? M.hedges[he1_op].face: NO_ID;
      const size_t q2 = (he2_op != NO_ID) ? M.hedges[he2_op].face: NO_ID;
      const size_t q3 = (he3_op != NO_ID) ? M.hedges[he3_op].face: NO_ID;
      const bool q1in = (q1 != NO_ID && quads.find(q1) != quads.end());
      const bool q2in = (q2 != NO_ID && quads.find(q2) != quads.end());
      const bool q3in = (q3 != NO_ID && quads.find(q3) != quads.end());
      if        ( q1in &&  q2in && !q3in) { /* minus two vertices on the bdr */
        size_t nv1 = M.vertex(he1,0);
        size_t nv2 = M.vertex(he1,1);
        if (rejectNewSings && (M.vertices[nv1].isSingularity || M.vertices[nv2].isSingularity)) {
          if (DBG_VERBOSE) {DBG("flip -2v rejected because would include singularity", i, info.nq);}
          return false;
        }
        info.nvs = {NO_ID,NO_ID,NO_ID,NO_ID};
        size_t i_prev_prev = (i + hes.size() - 2)%hes.size();
        hes[i_prev_prev] = he3;
        auto it0 = std::find(hes.begin(),hes.end(),he0_op);
        hes.erase(it0);
        auto it1 = std::find(hes.begin(),hes.end(),he1_op);
        hes.erase(it1);
        if (DBG_VERBOSE) {DBG("flip -2v", i, info.nq); } 
      } else if ( q1in && !q2in &&  q3in) { /* minus two vertices on the bdr */
        size_t nv1 = M.vertex(he0_op,0);
        size_t nv2 = M.vertex(he0_op,1);
        if (rejectNewSings && (M.vertices[nv1].isSingularity || M.vertices[nv2].isSingularity)) {
          if (DBG_VERBOSE) {DBG("flip -2v rejected because would include singularity", i, info.nq);}
          return false;
        }
        info.nvs = {NO_ID,NO_ID,NO_ID,NO_ID};
        size_t i_prev = (i + hes.size() - 1)%hes.size();
        hes[i_prev] = he2;
        auto it0 = std::find(hes.begin(),hes.end(),he0_op);
        hes.erase(it0);
        auto it3 = std::find(hes.begin(),hes.end(),he3_op);
        hes.erase(it3);
        if (DBG_VERBOSE) {DBG("flip -2v", i, info.nq); } 
      } else if (!q1in &&  q2in &&  q3in) { /* minus two vertices on the bdr */
        size_t nv1 = M.vertex(he3,0);
        size_t nv2 = M.vertex(he3,1);
        if (rejectNewSings && (M.vertices[nv1].isSingularity || M.vertices[nv2].isSingularity)) {
          if (DBG_VERBOSE) {DBG("flip -2v rejected because would include singularity", i, info.nq);}
          return false;
        }
        info.nvs = {NO_ID,NO_ID,NO_ID,NO_ID};
        hes[i] = he1;
        auto it2 = std::find(hes.begin(),hes.end(),he2_op);
        hes.erase(it2);
        auto it3 = std::find(hes.begin(),hes.end(),he3_op);
        hes.erase(it3);
        if (DBG_VERBOSE) {DBG("flip -2v", i, info.nq); } 
      } else if (q1in && q2in && q3in){
        size_t nv0 = M.hedges[he0].vertex;
        size_t nv1 = M.hedges[he0].vertex;
        size_t nv2 = M.hedges[he0].vertex;
        size_t nv3 = M.hedges[he0].vertex;
        if (rejectNewSings && (M.vertices[nv0].isSingularity || M.vertices[nv1].isSingularity
             || M.vertices[nv2].isSingularity || M.vertices[nv3].isSingularity)) {
          if (DBG_VERBOSE) {DBG("flip closing hole rejected because would include singularity", i, info.nq);}
          return false;
        }
        info.nvs = {nv0,nv1,nv2,nv3};
        auto it0 = std::find(hes.begin(),hes.end(),he0_op);
        hes.erase(it0);
        auto it1 = std::find(hes.begin(),hes.end(),he1_op);
        hes.erase(it1);
        auto it2 = std::find(hes.begin(),hes.end(),he2_op);
        hes.erase(it2);
        auto it3 = std::find(hes.begin(),hes.end(),he3_op);
        hes.erase(it3);
        std::vector<size_t> hes_stack = hes;
        bool oks = orderedHalfEdgesFromStack(this->M, hes_stack, this->hes);
        if (!oks) {
          Msg::Error("failed to determine sides from %li boundary half edges (%li quads)", hes_stack.size(), quads.size());
          info.nq = NO_ID;
          return false;
        }
        if (DBG_VERBOSE) {DBG("flip closing hole (may be too slow ?)", i, info.nq); } 
      } else if ( q1in && !q2in && !q3in) { /* same number of vertices on the bdr */
        /* Check we are not creating a non-manifold edge boundary */
        const size_t nv = M.hedges[he2].vertex;
        const int val = valenceInsideQuads(M, quads, nv);
        if (val > 0) {
          if (DBG_VERBOSE) {DBG("no flip <>1v because", i, info.nq, nv, val);}
          info.nq = NO_ID;
          return false;
        }
        const size_t nvIn = M.hedges[he0].vertex;
        if (rejectNewSings && M.vertices[nvIn].isSingularity) {
          if (DBG_VERBOSE) {DBG("no flip <>1v because would include sing", i, info.nq, nvIn, val);}
          info.nq = NO_ID;
          return false;
        }
        info.nvs = {NO_ID,NO_ID,NO_ID,NO_ID};
        size_t i_prev = (i + hes.size() - 1)%hes.size();
        hes[i_prev] = he2;
        hes[i] = he3;
        if (DBG_VERBOSE) {DBG("flip <>1v (1)", i, info.nq, nv); } 
      } else if (!q1in && !q2in &&  q3in) { /* same number of vertices on the bdr */
        /* Check we are not creating a non-manifold edge boundary */
        const size_t nv = M.hedges[he1].vertex;
        const int val = valenceInsideQuads(M, quads, nv);
        if (val > 0) {
          if (DBG_VERBOSE) {DBG("no flip <>1v because", i, info.nq, nv, val);}
          info.nq = NO_ID;
          return false;
        }
        const size_t nvIn = M.hedges[he0_op].vertex;
        if (rejectNewSings && M.vertices[nvIn].isSingularity) {
          if (DBG_VERBOSE) {DBG("no flip <>1v because would include sing", i, info.nq, nvIn, val);}
          info.nq = NO_ID;
          return false;
        }
        info.nvs = {NO_ID,NO_ID,NO_ID,NO_ID};
        size_t i_next = (i + 1)%hes.size();
        hes[i] = he1;
        hes[i_next] = he2;
        if (DBG_VERBOSE) {DBG("flip <>1v (2)", i, info.nq, nv);}
      } else if (!q1in && !q2in && !q3in) { /* two additional vertices on the bdr */
        /* Check we are not creating a non-manifold edge boundary */
        const size_t nv1 = M.hedges[he1].vertex;
        const int val1 = valenceInsideQuads(M, quads, nv1);
        if (val1 > 0) {
          if (DBG_VERBOSE) {DBG("no flip +2v because", i, info.nq, nv1, val1);}
          info.nq = NO_ID;
          return false;
        }
        const size_t nv2 = M.hedges[he2].vertex;
        const int val2 = valenceInsideQuads(M, quads, nv2);
        if (val2 > 0) {
          if (DBG_VERBOSE) {DBG("no flip +2v because", i, info.nq, nv2, val2);}
          info.nq = NO_ID;
          return false;
        }
        if (rejectNewSings) {
          const size_t v0 = M.vertex(he0,0);
          if (M.vertices[v0].isSingularity && valenceOutsideQuads(M, quads, v0) == 2) {
            /* Would be concave corner around singularity, reject */
            if (DBG_VERBOSE) {DBG("no flip +2v because would be concave corner at sing.", i, info.nq);}
            return false;
          }
          const size_t v1 = M.vertex(he0,1);
          if (M.vertices[v1].isSingularity && valenceOutsideQuads(M, quads, v1) == 2) {
            /* Would be concave corner around singularity, reject */
            if (DBG_VERBOSE) {DBG("no flip +2v because would be concave corner at sing.", i, info.nq);}
            return false;
          }
        }
        /* Do the flip */
        hes[i] = he1;
        hes.insert(hes.begin()+i+1,{he2,he3});
        if (DBG_VERBOSE) {DBG("flip +2v", i, info.nq);}
      
        } else {
        if (DBG_VERBOSE) {DBG("flip config not supported", i, info.nq, he0, he1, he2, he3, q1in, q2in, q3in);}
        info.nq = NO_ID;
        return false;
      }
      quads.insert(info.nq);
      return true;
    }

    int updateSides() {
      // if (DBG_VERBOSE) {DBG("updateSides ...");}
      side.resize(hes.size());
      std::unordered_map<size_t,int> val;
      for (size_t f: quads) {
        size_t he = M.faces[f].he;
        for (size_t lv = 0; lv < 4; ++lv) {
          size_t v = M.hedges[he].vertex;
          val[v] += 1;
          he = M.hedges[he].next;
        }
      }

      std::unordered_set<size_t> corners;
      for (const auto& kv: val) {
        if (kv.second == 1) corners.insert(kv.first);
      }

      // if (DBG_VERBOSE) {DBG(corners);}
      int sideNo = -1;
      for (size_t i = 0; i < hes.size(); ++i) {
        const size_t he0 = hes[i];
        const size_t v0 = M.vertex(he0,0);
        if (corners.find(v0) == corners.end()) {
          continue;
        }
        for (size_t j = 0; j < hes.size(); ++j) {
          const size_t he_pos = (i+j)%hes.size();
          const size_t he = hes[he_pos];
          size_t v1 = M.vertex(he,0);
          if (corners.find(v1) != corners.end()) {
            sideNo += 1;
          }
          side[he_pos] = sideNo;
        }
        break;
      }

      return sideNo+1;
    }

  };

  void geolog_fcavity(const FCavity& cav, const std::string& name) {
    for (size_t i = 0; i < cav.hes.size(); ++i) {
      geolog_halfedge(cav.M, cav.hes[i], double(cav.side[i]), name);
    }
    for (size_t f: cav.quads) {
      geolog_face(cav.M, f, 1., name);
    }
    for (size_t v = 0; v < cav.M.vertices.size(); ++v) if (cav.M.vertices[v].isSingularity) {
      bool b;
      GeoLog::add(cav.M.vertices[v].p, double(cav.M.vertexFaceValence(v, b)), name);
    }
    GeoLog::flush();
  }

  bool cavityIsRemeshable(const FCavity& cav, double& irregularityMeasure) {
    if (cav.hes.size() != cav.side.size()) {
      Msg::Error("wrong side vector size");
      return false;
    }
    std::vector<size_t> npts;
    for (size_t i = 0; i < cav.hes.size(); ++i) {
      size_t s = (size_t) cav.side[i];
      if (s >= npts.size()) {
        npts.resize(s+1,0);
        if (s >= 5) return false; /* keep cavity with less than 5 sides */
      }
      npts[s] += 1;
    }
    /* Nb edges -> nb points (including bdr) */
    for (size_t i = 0; i < npts.size(); ++i) npts[i] += 1;

    std::pair<size_t,int> patternNoAndRot;
    size_t Ncorners = npts.size();
    bool meshable = patchIsRemeshableWithQuadPattern(Ncorners, npts, patternNoAndRot, irregularityMeasure);
    return meshable;
  }

  struct Gardener {
    /* Gardener is used to manage the growth of cavities
     * Can be re-used as long as the GFace is not changed */

    /* Data */
    MeshHalfEdges& M;
    vector<int> valence;
    vector<bool> vOnBoundary;
    FCavity* current;
    vector<int> valenceInCavity;
    int cavityTargetNbOfSides;
    /* singularities (flagged irregular) strictly inside */
    std::unordered_set<size_t> sings;
    /* singularities (flagged irregular) strictly on the cavity on the cavity boundary */
    std::unordered_set<size_t> singsBdr;
    /* irregular vertices (not sing.) inside, including bdr */
    std::unordered_set<size_t> irregular;
    /* when growing, keep last valid remeshable cavity */
    FCavity lastCav;
    size_t lastNbIrregular;
    double lastIrregularity;

    /* Methods */
    Gardener(MeshHalfEdges& M_) : M(M_), current(NULL), lastCav(M), lastIrregularity(DBL_MAX) {
      /* Initialize data from MeshHalfEdges */
      valence.resize(M.vertices.size(),0);
      valenceInCavity.resize(M.vertices.size());
      vOnBoundary.resize(M.vertices.size(),false);
      for (size_t i = 0; i < M.faces.size(); ++i) {
        size_t he = M.faces[i].he;
        do {
          size_t v2 = M.hedges[he].vertex;
          valence[v2] += 1;
          if (M.hedges[he].opposite == NO_ID) {
            size_t v1 = M.vertex(he,0);
            if (!vOnBoundary[v1]) vOnBoundary[v1] = true;
            if (!vOnBoundary[v2]) vOnBoundary[v2] = true;
          }
          he = M.next(he);
        } while (he != M.faces[i].he);
      }
    }

    bool setCavity(FCavity& cav) {
      if (cav.quads.size() == 0 || cav.hes.size() == 0) return false;
      current = &cav;

      /* Clean stuff of the previous current cavity */
      std::fill(valenceInCavity.begin(),valenceInCavity.end(),0);
      sings.clear();
      irregular.clear();
      singsBdr.clear();
      cavityTargetNbOfSides = 0;
      lastNbIrregular = 0;
      lastCav.hes.clear();
      lastCav.side.clear();
      lastCav.quads.clear();

      std::vector<size_t> asings;
      for (size_t i: cav.quads) {
        size_t he = M.faces[i].he;
        do {
          size_t v2 = M.hedges[he].vertex;
          valenceInCavity[v2] += 1;
          if (M.vertices[v2].isSingularity) {
            asings.push_back(v2);
          } else if (vOnBoundary[v2] && valence[v2] != 2) {
            irregular.insert(v2);
          } else if (!vOnBoundary[v2] && valence[v2] != 4) {
            irregular.insert(v2);
          }
          he = M.next(he);
        } while (he != M.faces[i].he);
      }
      if (cav.quads.size() == 3) {
        cavityTargetNbOfSides = 3;
      } else if (cav.quads.size() == 4 || cav.quads.size() == 1) {
        cavityTargetNbOfSides = 4;
      } else if (cav.quads.size() == 5) {
        cavityTargetNbOfSides = 5;
      } else {
        cavityTargetNbOfSides = 0;
      }
      /* Only add singularities if strictly inside */
      sort_unique(asings);
      for (size_t v: asings) if (valenceInCavity[v] == valence[v]) {
        sings.insert(v);
      } else {
        singsBdr.insert(v);
      }
      return true;
    }

    bool isConvex() const {
      if (current == NULL) return false;
      FCavity& cav = *current;
      for (size_t i = 0; i < cav.hes.size(); ++i) {
        size_t he = cav.hes[i];
        size_t v = M.hedges[he].vertex;
        int valOutside = valence[v] - valenceInCavity[v];
        if (!vOnBoundary[v] && valOutside == 1) return false;
        // if (vOnBoundary[v] && valOutside == 0 && valence[v] >= 3) {
        //   /* Check if corner */
        //   MVertex* mv = M.vertices[v].ptr;
        //   GVertex* gv = dynamic_cast<GVertex*>(mv->onWhat());
        //   if (gv != NULL) {
        //     /* Concave corner absorbed, bad */
        //     return false;
        //   }
        // }
      }
      return true;
    }

    void markNewQuad(size_t nq) {
      /* Call this to update the Gardener data when
       * a new quad is added to the current cavity */
      size_t heq = M.faces[nq].he;
      do {
        size_t v2q = M.hedges[heq].vertex;
        valenceInCavity[v2q] += 1;
        if (M.vertices[v2q].isSingularity) {
          if (valenceInCavity[v2q] == valence[v2q]) {
            if (DBG_VERBOSE) {DBG("new sing inside, bad ! abort", nq, v2q);}
            abort();
            sings.insert(v2q);
          } else {
            singsBdr.insert(v2q);
          }
        } else if (vOnBoundary[v2q] && valence[v2q] != 2) {
          irregular.insert(v2q);
        } else if (!vOnBoundary[v2q] && valence[v2q] != 4) {
          irregular.insert(v2q);
        }
        heq = M.next(heq);
      } while (heq != M.faces[nq].he);
    }


    bool getFlipHalfEdgeCandidates(std::vector<size_t>& candidates) {
      if (current == NULL) return false;
      FCavity& cav = *current;

      candidates.clear();
      candidates.reserve(cav.hes.size());

      /* Forbid half-edges on the same side of a singularity,
       * or of a concave corner */
      std::unordered_set<size_t> limits = singsBdr;
      if (irregular.size() > 0) {
        for (size_t v: irregular) if (vOnBoundary[v] && valence[v] > 2) {
          /* Check if CAD corner */
          MVertex* ptr = cav.M.vertices[v].ptr;
          GVertex* gv = dynamic_cast<GVertex*>(ptr->onWhat());
          if (gv != NULL) {
            limits.insert(v);
          }
        }
      }

      if (limits.size() == 0) {
        for (size_t k = 0; k < cav.hes.size(); ++k) {
          if (M.opposite(cav.hes[k]) != NO_ID) {
            candidates.push_back(cav.hes[k]);
          }
        }
      } else{
        /* Get half edges on boundary of the cavity which are not on the
         * side of singularity */
        std::unordered_set<size_t> forbidden;
        std::vector<size_t> hesOnLimit(100);

        bool show = false;
        // for (size_t bs: limits) {
        //   if (vOnBoundary[bs] && valence[bs] != 2) {
        //     show = true; break;
        //   }
        // }

        for (size_t bs: limits) {
          hesOnLimit.clear();
          M.vertexHalfEdges(bs, hesOnLimit);
          size_t heInit = NO_ID;
          size_t iInit = NO_ID;
          for (size_t he: hesOnLimit) {
            size_t he_op = M.opposite(he);
            if (he_op == NO_ID) continue;
            auto it = std::find(cav.hes.begin(),cav.hes.end(),he);
            if (it != cav.hes.end()) {
              heInit = he;
              iInit = (size_t) (it - cav.hes.begin());
              break;
            } else { /* Try opposite one */
              it = std::find(cav.hes.begin(),cav.hes.end(),he_op);
              if (it != cav.hes.end()) {
                heInit = he_op;
                iInit = (size_t) (it - cav.hes.begin());
                break;
              }
            }
          }
          if (heInit == NO_ID || iInit == NO_ID) {
            Msg::Error("weird, getFlipHalfEdgeCandidates, limit vertex (sing or concave) = %li, hesOnLimit.size() = %li, but he not found on cavity bdr",
                bs, hesOnLimit.size());
            return false;
            show = true;
            GeoLog::flush();
            gmsh::view::add("---");
            GeoLog::add(M.vertices[bs].p, double(hesOnLimit.size()),"!gfhec_not_on_bdr");
            continue;
          }
          size_t i = iInit;
          while (true) {
            size_t he = cav.hes[i];
            forbidden.insert(he);
            size_t v2 = M.vertex(he,1);
            int valOutside2 = valence[v2] - valenceInCavity[v2];
            if (valOutside2 == 1 || valenceInCavity[v2] == 1) {
              break;
            }
            i = (i + 1)%cav.hes.size();
            if (i == iInit) break;
          }
          i = iInit;
          while (true) {
            size_t he = cav.hes[i];
            forbidden.insert(he);
            size_t v1 = M.vertex(he,0);
            int valOutside1 = valence[v1] - valenceInCavity[v1];
            if (valOutside1 == 1 || valenceInCavity[v1] == 1) {
              break;
            }
            i = (i - 1 + cav.hes.size())%cav.hes.size();
            if (i == iInit) break;
          }
        }
        for (size_t k = 0; k < cav.hes.size(); ++k) {
          size_t he = cav.hes[k];
          if (M.opposite(cav.hes[k]) != NO_ID) {
            auto it = forbidden.find(he);
            if (it == forbidden.end()) {
              candidates.push_back(he);
              if (show) geolog_halfedge(M, he, 0, "allowed");
            }
          }
        }
        if (show) {
          for (size_t he: forbidden) {
            geolog_halfedge(M, he, 1, "forbidden");
          }
          geolog_fcavity(cav, "cavity");
          for (size_t bs: limits) {
            GeoLog::add(M.vertices[bs].p, double(hesOnLimit.size()),"limits");
            hesOnLimit.clear();
            M.vertexHalfEdges(bs, hesOnLimit);
            for (size_t he: hesOnLimit) {
              geolog_halfedge(M, he, 0., "hesOnLimit");
            }
          }
          GeoLog::flush();
        }
      }

      return true;
    }

    bool convexify() {
      if (current == NULL) return false;
      FCavity& cav = *current;
      bool running = true;
      FlipInfo info;
      while (running) {
        running = false;
        for (size_t i = 0; i < cav.hes.size(); ++i) {
          size_t he = cav.hes[i];
          size_t v1 = M.vertex(he,0);
          size_t v2 = M.vertex(he,1);
          int valOutside1 = valence[v1] - valenceInCavity[v1];
          int valOutside2 = valence[v2] - valenceInCavity[v2];
          if ((!vOnBoundary[v1] &&valOutside1 == 1) || (!vOnBoundary[v2] && valOutside2 == 1)) {
            bool ok = cav.growByFlip(i, info);
            if (ok) {
              running = true;
              markNewQuad(info.nq);
            }
          }
        }
      }
      return true;
    }

    bool growIsotropic(size_t N) {
      if (current == NULL) return false;
      FCavity& cav = *current;
      srand(0);
      bool running = true;
      size_t nb = 0;
      FlipInfo info;
      while (running && nb < N) {
        running = false;
        /* Try random flip (up to hes.size() tries) */
        for (size_t k = 0; k < cav.hes.size(); ++k) {
          size_t i = (rand() % static_cast<size_t>(cav.hes.size()));
          size_t he = cav.hes[i];
          size_t v = M.hedges[he].vertex;
          if (DBG_VERBOSE) {DBG(nb,N,"---",i,he,v);}
          bool ok = cav.growByFlip(i, info);
          if (ok) {
            nb += 1;
            running = true;
            markNewQuad(info.nq);
            break;
          }
        }
      }
      return true;
    }

    bool growMaximal() {
      if (current == NULL) return false;
      FCavity& cav = *current;
      srand(0);
      bool running = true;
      size_t nb = 0;
      FlipInfo info;
      std::vector<size_t> candidates;
      while (running) {
        running = false;
        bool okc = getFlipHalfEdgeCandidates(candidates);
        if (!okc) {
          Msg::Debug("growMaximal: failed to get flip half edge candidates");
          break;
        }
        for (size_t k = 0; k < candidates.size(); ++k) {
          size_t he = candidates[k];
          auto it = std::find(cav.hes.begin(),cav.hes.end(),he);
          if (it == cav.hes.end()) continue;
          size_t pos = (size_t) (it - cav.hes.begin());
          bool ok = cav.growByFlip(pos, info, true);
          if (ok) {
            nb += 1;
            running = true;
            markNewQuad(info.nq);
            bool okf = convexify();
            if (!okf) {
              Msg::Debug("growMaximal: failed to convexity, stop growth");
              running = false;
              break;
            }
          }
        }
        if (running) {
          bool convex = isConvex();
          if (!convex) {
            Msg::Debug("growMaximal: cavity not convex, stop growth");
            running = false;
            // geolog_fcavity(cav, "!convex");
            break;
          } else {
            size_t nbi = irregular.size();
            if (nbi > lastNbIrregular) {
              int nsides = cav.updateSides();
              if (nsides == (int) cavityTargetNbOfSides) {
                double currentCavityIrregularity = 0.;
                /* Check number of irregular outside cavity corners */
                size_t nbi_oc = 0;
                for (size_t v: irregular)  {
                  int valIn = valenceInsideQuads(cav.M,cav.quads,v);
                  if (valIn <= 2) continue;
                  nbi_oc += 1;
                  currentCavityIrregularity += std::pow(valIn-4,2);
                }
                if (nbi_oc == 0) continue; /* all irregular are cavity corners, no need to remesh */
                for (size_t v: sings) {
                  int valIn = valenceInsideQuads(cav.M,cav.quads,v);
                  currentCavityIrregularity += std::pow(valIn-4,2);
                }

                double irreg = DBL_MAX;
                bool remeshable = cavityIsRemeshable(cav, irreg);
                if (remeshable && irreg <= lastIrregularity && irreg < currentCavityIrregularity) {
                  /* Do not choose a pattern if strictly worse irregularity */
                  Msg::Debug("growMaximal: set remeshable cavity (%li quads, irregularity=%f, #irreg=%li)", cav.quads.size(), irreg, nbi_oc);
                  lastCav = cav;
                  lastNbIrregular = nbi;
                  lastIrregularity = irreg;
                }
              }
            }
          }
        }
      }
      if (lastNbIrregular > 0) {
        if (M.faces.size() == cav.quads.size() && lastNbIrregular == (size_t) cavityTargetNbOfSides) {
          /* cavity is full face, which is triangle or quad or pentagon with the right number
           * of irregular vertices at the corners */
          return false;
        }
        // geolog_fcavity(cav, "maxBeforeLast");
        cav = lastCav;
      } else {
        return false;
      }

      return true;
    }

  };

  // /* warning: indices (in start, adjacent[i], chord) are in the form 4*f+le */
  // bool buildChord(size_t start, const vector<array<size_t,4> >& adjacent, vector<size_t>& chord) {
  //   /* Init */
  //   std::unordered_set<size_t> visited;
  //   std::queue<size_t> Q;
  //   Q.push(start);
  //   visited.insert(start / 4);

  //   /* Propagation */
  //   while (Q.size() > 0) {
  //     size_t a = Q.front();
  //     Q.pop();
  //     chord.push_back(a);
  //     size_t f = a/4;
  //     size_t le = a%4;

  //     for (size_t k = 0; k < 2; ++k) {
  //       size_t le_cur = (k == 0) ? le : (le+2)%4;
  //       size_t a2 = adjacent[f][le_cur];
  //       if (a2 == NO_ID) continue;
  //       size_t f2 = a2 / 4;
  //       bool already_visited = (visited.find(f2) != visited.end());
  //       if (already_visited) continue;
  //       Q.push(a2);
  //       visited.insert(a2/4);
  //     }
  //   }

  //   sort_unique(chord);
  //   return chord.size() > 2;
  // }

  // inline si2 sorted(size_t v1, size_t v2) { if (v1 < v2) { return {v1,v2}; } else { return {v2,v1}; } }

  // struct DualGardener {
  //   vector<MElement*> quads;
  //   vector<array<size_t,4> > adjacent;
  //   vector<vector<size_t> > barrier; /* barrier = list of quads */
  //   vector<bool> isTrigger;
  //   unordered_map<size_t, vector<size_t> > trigger; /* quad -> barrier ids */


  //   bool init(const GFace* gf) {
  //     unordered_map<si2,si2,si2hash> vPairToQuads;
  //     quads.resize(gf->quadrangles.size());
  //     adjacent.resize(gf->quadrangles.size());
  //     for (size_t i = 0; i < gf->quadrangles.size(); ++i) {
  //       MElement* f = gf->quadrangles[i];
  //       quads[i] = f;
  //       adjacent[i] = {NO_ID,NO_ID,NO_ID,NO_ID};
  //       for (size_t le = 0; le < 4; ++le) {
  //         size_t v1 = f->getVertex(le)->getNum();
  //         size_t v2 = f->getVertex((le+1)%4)->getNum();
  //         si2 vPair = sorted(v1,v2);
  //         auto it = vPairToQuads.find(vPair);
  //         if (it == vPairToQuads.end()) {
  //           vPairToQuads[vPair] = {4*i+le,NO_ID};
  //         } else {
  //           if (it->second[1] == NO_ID) {
  //             size_t a2 = it->second[0];
  //             if (a2 == NO_ID) {
  //               Msg::Error("DualGardener init: should not happen");
  //               return false;
  //             }
  //             it->second[1] = 4*i+le;
  //             size_t f2 = a2 / 4;
  //             size_t le2 = a2 % 4;
  //             adjacent[i][le] = 4*f2+le2;
  //             adjacent[f2][le2] = 4*i+le;
  //           } else {
  //             Msg::Error("DualGardener init: non manifold quad mesh for face %i", gf->tag());
  //             return false;
  //           }
  //         }
  //       }
  //     }

  //     return true;
  //   }

  //   bool buildCavityBarriers(
  //       const std::unordered_set<MVertex*>& singularities,
  //       const std::unordered_set<MVertex*>& concaveCorners) {

  //     unordered_map<MVertex*,vector<MElement*> > v2e;
  //     for (size_t i = 0; i < quads.size(); ++i) {
  //       MElement* f = quads[i];
  //       for (size_t lv = 0; lv < 4; ++lv) {
  //         MVertex* v = f->getVertex(lv);
  //         auto it1 = singularities.find(v);
  //         if (it1 != singularities.end()) {
  //           v2e[v].push_back(f);
  //           continue;
  //         }
  //         auto it2 = concaveCorners.find(v);
  //         if (it2 != concaveCorners.end()) {
  //           v2e[v].push_back(f);
  //         }
  //       }
  //     }

  //     /* Build barriers */
  //     for (MVertex* v: concaveCorners) {
  //       auto it = v2e.find(v);

  //       if (it == v2e.end()) {
  //         Msg::Warning("no quads around concave corner ? weird");
  //         continue;
  //       } else if (it->second.size() == 3) {

  //       } else if (it->second.size() == 4) {
  //       } else {
  //         Msg::Warning("concave corner %li has %li adjacent quads", v->getNum(), it->second.size());
  //       }
  //     }

  //     return true;
  //   }
  // };


  bool convexifyQuads(const MeshHalfEdges& M, std::vector<size_t>& quadsVector) {
    std::unordered_set<size_t> quads;
    std::vector<size_t> hes;
    for (size_t f: quadsVector) quads.insert(f);
    bool okb = boundaryHalfEdgesFromQuads(M, quads, hes);
    if (!okb) {
      Msg::Error("convexify: failed to get boundary half edges from %li quads", quads.size());
      return false;
    }

    bool remaining = true;
    while (remaining) {
      remaining = false;
      vector<size_t> faces;
      vector<size_t> quadToAdd;
      vector<size_t> cands;
      for (size_t he: hes) {
        size_t v = M.hedges[he].vertex;
        M.vertexFaces(v, faces);
        int count = 0;
        cands.clear();
        for (size_t f: faces) {
          if (quads.find(f) != quads.end()) {
            count += 1;
          } else {
            cands.push_back(f);
          }
        }

        /* Do not grow cavity if other sing inside */
        if (M.vertices[v].isSingularity) {
          if (cands.size() == 1 && count == 2) { /* v is singularity but regular from the inside, do not grow */
            continue;
          } else {
            Msg::Info("convexify: vertex %i (num=%i) is singularity, don't grow", v, M.vertices[v].ptr->getNum());
            return false;
          }
        }

        /* Concave vertex */
        if (cands.size() == 1) {
          quadToAdd.push_back(cands[0]);
        } else {
          // bool onBdr = false;
          // if (M.vertexFaceValence(v,onBdr) != 4 && !onBdr) {
          //   /* Irregular vertex on cavity boundary, put him inside */
          //   for (size_t f: cands) {
          //     quadToAdd.push_back(f);
          //   }
          // }
        }
      }
      if (quadToAdd.size() == 0) break;
      remaining = true;
      sort_unique(quadToAdd);

      for (size_t f: quadToAdd) {
        quads.insert(f);
        size_t he = M.faces[f].he;
        do {
          hes.push_back(he);
          he = M.next(he);
        } while (he != M.faces[f].he);
      }
      removeInteriorHalfEdges(M, hes);
    }

    quadsVector.clear();
    quadsVector.reserve(quads.size());
    for (size_t f: quads) quadsVector.push_back(f);

    return true;
  }

  inline bool vertexIsRegular(const std::vector<int>& valence, const std::vector<bool>& onBdr, size_t v) {
    return (onBdr[v] && valence[v] == 2) || (!onBdr[v] && valence[v] == 4);
  };

  void geolog_elements(const std::vector<MElement*>& elts, const std::string& name) {
    for (MElement* f: elts) {
      vector<array<double,3> > pts(f->getNumVertices());
      for (size_t i = 0; i < pts.size(); ++i) {
        pts[i] = SVector3(f->getVertex(i)->point());
      }
      vector<double> values(pts.size(),double(f->getNum()));
      GeoLog::add(pts,values,name);
    }
  }

  void geolog_closed_curve(const std::vector<MVertex*>& bnd, const std::string& name) {
    for (size_t i = 0; i < bnd.size(); ++i) {
      SVector3 p1 = bnd[i]->point();
      SVector3 p2 = bnd[(i+1)%bnd.size()]->point();
      vector<array<double,3>> pts = {p1,p2};
      vector<double> values = {double(i),double(i+1)};
      GeoLog::add(pts,values,name);
    }
  }


  inline bool vertexOnSeamCurve(GFace* gf, MVertex* v) {
    GEdge* ge = dynamic_cast<GEdge*>(v->onWhat());
    if (ge == NULL) return false;
    if (ge->isSeam(gf) && ge->faces().size() == 1) return true;
    return false;
  }

  const int CORNER = 1;
  const int CURVE = 2;
  const int SURFACE = 3;

  inline int vertexSupport(GFace* gf, MVertex* v) {
    GVertex* gv = dynamic_cast<GVertex*>(v->onWhat());
    if (gv != NULL) return CORNER;
    GEdge* ge = dynamic_cast<GEdge*>(v->onWhat());
    if (ge != NULL) return CURVE;
    GFace* gf2 = dynamic_cast<GFace*>(v->onWhat());
    if (gf2 != NULL) return SURFACE;
    return 0;
  }

  inline bool vertexOnBoundary(GFace* gf, MVertex* v) {
    int vs = vertexSupport(gf,v);
    return (vs == CORNER || vs == CURVE);
  }

  void removeElement(GFace* gf, MElement* e, std::unordered_map<MVertex *, std::vector<MElement *> >& adj) {
    if (e == NULL) return;
    for (size_t i=0;i<e->getNumVertices();i++){
      MVertex* v = e->getVertex(i);
      if (v == NULL) continue;
      auto it = adj.find(v);
      if (it != adj.end()){
        auto it2 = std::find(it->second.begin(),it->second.end(),e);
        if (it2 != it->second.end()) it->second.erase(it2);
      }
    }
    MQuadrangle *q = dynamic_cast<MQuadrangle*>(e);	      
    if (!q)Msg::Error ("A non quad is present in the list of quad of face %lu",gf->tag());
    auto it = std::find(gf->quadrangles.begin(),gf->quadrangles.end(), q);
    if (it != gf->quadrangles.end()) {
      gf->quadrangles.erase(it);
    }
    delete e;
    e = NULL;
  }

  void removeVertex(MVertex* v, std::unordered_map<MVertex *, std::vector<MElement *> >& adj) {
    if (v == NULL) return;
    auto it = adj.find(v);
    if (it != adj.end()){
      adj.erase(it);
    }
    GFace* gf = dynamic_cast<GFace*>(v->onWhat());
    if (gf != NULL) {
      auto it = std::find(gf->mesh_vertices.begin(),gf->mesh_vertices.end(), v);
      if (it != gf->mesh_vertices.end()) {
        gf->mesh_vertices.erase(it);
      }
    } else {
      GEdge* ge = dynamic_cast<GEdge*>(v->onWhat());
      if (ge != NULL) {
        auto it = std::find(ge->mesh_vertices.begin(),ge->mesh_vertices.end(), v);
        if (it != ge->mesh_vertices.end()) {
          ge->mesh_vertices.erase(it);
        }
      } else {
        GVertex* gv = dynamic_cast<GVertex*>(v->onWhat());
        if (gv != NULL) {
          auto it = std::find(gv->mesh_vertices.begin(),gv->mesh_vertices.end(), v);
          if (it != gv->mesh_vertices.end()) {
            gv->mesh_vertices.erase(it);
          }
        }
      }
    }
    delete v;
    v = NULL;
  }

  inline void addToAdjacencyList(GFace* gf, MElement*e, std::unordered_map<MVertex *, std::vector<MElement *> >& adj)
  {
    for (size_t i=0;i<e->getNumVertices();i++){
      MVertex* v = e->getVertex(i);
      adj[v].push_back(e);
    }
  }

  int vertexIdealValence(MVertex* v, const std::unordered_map<MVertex *, double>& vAngle) {
    auto it = vAngle.find(v);
    if (it == vAngle.end()) return 4;
    int ival = int(clamp(std::round(4. * it->second / (2. * M_PI)),1.,4.));
    return ival;
  }

  bool slowVerifyMeshIsValid(GFace* gf) {
    Msg::Debug("slow, verify mesh of face %i ...", gf->tag());
    std::unordered_map<MVertex *, std::vector<MElement *> > adj;
    std::map<array<size_t,2>, size_t> edgeVal;
    for (MQuadrangle* f: gf->quadrangles) {
      for (size_t lv = 0; lv < 4; ++lv) {
        MVertex* v = f->getVertex(lv);
        adj[v].push_back(f);
        MVertex* v2 = f->getVertex((lv+1)%4);
        if (v->getNum() < v2->getNum()) {
          array<size_t,2> vpair = {v->getNum(),v2->getNum()};
          edgeVal[vpair] += 1;
        } else {
          array<size_t,2> vpair =  {v2->getNum(),v->getNum()};
          edgeVal[vpair] += 1;
        }
      }
    }

    bool ok = true;
    for (auto& kv: edgeVal) {
      if (kv.second > 2) {
        Msg::Error("slowVerifyMeshIsValid | edge (%i,%i) non manifold, valence =  %i", kv.first[0],kv.first[1], kv.second);
        ok = false;
        if (DBG_VIZU) {
          size_t num1 = kv.first[0];
          size_t num2 = kv.first[1];
          MVertex* v1 = gf->getMeshVertex(num1);
          MVertex* v2 = gf->getMeshVertex(num2);
          if (v1 == NULL || v2 == NULL) {
            Msg::Error("  edge vertices not found in GFace");
            continue;
          } 
          SVector3 p1 = v1->point();
          SVector3 p2 = v2->point();
          vector<vec3> pts = {p1,p2};
          vector<double> values(2,double(kv.second));
          GeoLog::add(pts,values,"!non_manifold_edge");
        }
        continue;
      }
    }

    for (auto& kv: adj) {
      MVertex* v = kv.first;
      GFace* vgf = dynamic_cast<GFace*>(v->onWhat());
      if (vgf != NULL) {
        auto it = std::find(gf->mesh_vertices.begin(),gf->mesh_vertices.end(),v);
        if (it == gf->mesh_vertices.end()) {
          Msg::Error("slowVerifyMeshIsValid | vertex %i not found in GFace", v->getNum());
          continue;
        }
      }
      vector<MElement*> quads = kv.second;
      std::vector<MVertex*> bnd;
      bool okb = buildBoundary(quads.begin(),quads.end(), bnd);
      if (!okb) {
        GVertex* gv = dynamic_cast<GVertex*>(v->onWhat());
        if (gv != NULL) {
          Msg::Warning("slowVerifyMeshIsValid | failed to build boundary at vertex %i (on CAD corner, may be normal if not manifold)", v->getNum());
          continue;
        } else {
          Msg::Error("slowVerifyMeshIsValid | failed to build boundary at vertex %i (not on CAD corner, so should not happen)", v->getNum());
          ok = false;
          if (DBG_VIZU) {
            geolog_elements(quads, "!invalid_"+std::to_string(quads.size())+"quads");
            GeoLog::add(SVector3(v->point()),0.,"!failed_one_ring");
            GeoLog::flush();
          }
          continue;
        }
      }
      if (bnd.size() > 0 && bnd.back() == bnd.front()) bnd.resize(bnd.size()-1);
    }

    for (MVertex* v: gf->mesh_vertices) {
      size_t num = v->getNum();
      if (num == 0) Msg::Error("num should be >0 ?");
      auto it = adj.find(v);
      if (it == adj.end()) {
        Msg::Error("slowVerifyMeshIsValid | vertex %i has no adjacent quads", v->getNum());
        ok = false;
      } else if (it->second.size() == 0) {
        Msg::Error("slowVerifyMeshIsValid | vertex %i has no adjacent quads", v->getNum());
        ok = false;
      }
    }
    if (DBG_VIZU && !ok) GeoLog::flush();
    return ok;
  }

  double irregularityEnergyOfCavity(GFace* gf,
      const std::vector<MVertex*>& bnd,
      const std::vector<int>& bndIdealValence,
      const vector<std::pair<int,int> >& bndAllowedValenceRange,
      const std::vector<MVertex*>& inside,
      const std::vector<MElement*>& quads)  {
    std::unordered_map<MVertex*,int> qval;
    for (MElement* f: quads) for (size_t lv = 0; lv < f->getNumVertices(); ++lv) {
      MVertex* v = f->getVertex(lv);
      qval[v] += 1;
    }
    double energy = 0.;
    for (size_t i = 0; i < bnd.size(); ++i) {
      auto it = qval.find(bnd[i]);
      if (it == qval.end()) {
        Msg::Error("irregularityEnergyOfCavity | bnd vertex %li not found in qval", bnd[i]->getNum());
        continue;
      }
      double val = double(it->second);
      energy += std::pow(val-bndIdealValence[i],2);
      if (bndAllowedValenceRange[i].first == bndAllowedValenceRange[i].second) {
        if (val != bndAllowedValenceRange[i].first) {
          energy += 1000. * std::abs(val-bndAllowedValenceRange[i].first); /* defect penalty */
        }
      }
    }
    for (MVertex* v: inside) {
      auto it = qval.find(v);
      if (it == qval.end()) {
        Msg::Error("irregularityEnergyOfCavity | interior vertex %li not found in qval", v);
        continue;
      }
      double val = double(it->second);
      energy += std::pow(val-4.,2);
      if (val > 5) energy += 1000. * (val-5); /* defect penalty */
    }
    return energy;
  }

  double irregularityEnergyOnRing(GFace* gf, MVertex* v, 
      const std::unordered_map<MVertex *, std::vector<MElement *> >& adj,
      const std::unordered_map<MVertex *, double>& vAngle
      ) {
    int vs = vertexSupport(gf,v);
    const std::vector<MElement*> quads = adj.at(v);
    std::set<MVertex*> bnd; /* not ordered */
    for (MElement* f: quads) for (size_t lv = 0; lv < f->getNumVertices(); ++lv) {
      MVertex* v2 = f->getVertex(lv);
      if (v2 != v) bnd.insert(v2);
    }
    double irregularity = 0.;
    for (MVertex* v2: bnd) {
      int vs2 = vertexSupport(gf,v2);
      int val = adj.at(v2).size();
      if (vs2 == CORNER || vs2 == CURVE) {
        int ival = vertexIdealValence(v2, vAngle);
        irregularity += std::pow(double(val-ival),2);
      } else if (vs == SURFACE) {
        irregularity += std::pow(double(val-4),2);
      }
    }
    return irregularity;
  }

  bool remeshableVertexProperties(
      GFace* gf, MVertex* v,
      const std::unordered_map<MVertex *, std::vector<MElement *> >& adj,
      const std::unordered_map<MVertex *, double>& vAngle,
      int& ideal,
      vector<MElement*>& quads
      ) {
    int vs = vertexSupport(gf,v);
    quads = adj.at(v); /* copy */
    /* Ideal quad valence on the vertex */
    ideal = 4;
    if (vs == CORNER) {
      ideal = vertexIdealValence(v, vAngle);
    } else if (vs == CURVE) {
      ideal = 2;
    }

    /* Existing adjacent quads, check if current config is defect or not */
    if (vs == CORNER && quads.size() == (size_t) ideal) return false;
    if (vs == CURVE && quads.size() == 2) return false;
    if (vs == SURFACE && (quads.size() == 4 || quads.size() == 5)) return false;

    bool growCavityAroundQuad = false;

    if (vs == SURFACE && quads.size() == 3) { /* Check if there is a diamond */
      for (MElement* f: quads) {
        for (size_t lv = 0; lv < 4; ++lv) {
          MVertex* v2 = f->getVertex(lv);
          if (v2 == v) { /* Check others */
            MVertex* v2_prev = f->getVertex((lv+4-1)%4);
            MVertex* v2_op = f->getVertex((lv+2)%4);
            MVertex* v2_next = f->getVertex((lv+1)%4);
            if (adj.at(v2_prev).size() > 3 
                && adj.at(v2_op).size() == 3 
                && adj.at(v2_next).size() > 3) {
              if (vertexOnBoundary(gf,v2_prev) || vertexOnBoundary(gf, v2_op) || vertexOnBoundary(gf, v2_next)) continue;
              /* Found a diamond ! */
              quads = {f};
              growCavityAroundQuad = true;
              break;
            }
          }
        }
        if (growCavityAroundQuad) break;
      }
      if (!growCavityAroundQuad) return false; /* No diamond, keep current valence 3 vertex */
    }

    /* For valence 1 vertex on curves, need to grow the cavity a bit */
    if (quads.size() == 1 && ((vs == CORNER && ideal >= 2) || (vs == CURVE && ideal >= 2))) {
      growCavityAroundQuad = true;
    }
    
    if (growCavityAroundQuad) {
      for (size_t lv = 0; lv < 4; ++lv) {
        MVertex* v2 = quads[0]->getVertex(lv);
        auto it = adj.find(v2);
        if (it != adj.end()) {
          for (MElement* f2: it->second) {
            quads.push_back(f2);
          }
        }
      }
      sort_unique(quads);
    }

    return true;
  }


  size_t initPriorityQueueWithDefects(
      int pass,
      GFace* gf,
      const std::unordered_map<MVertex *, std::vector<MElement *> >& adj,
      const std::unordered_map<MVertex *, double>& vAngle,
      std::priority_queue<std::pair<double,MVertex*>, std::vector<std::pair<double,MVertex*> > >& Q) {
    for (auto& kv: adj) {
      MVertex* v = kv.first;
      int vs = vertexSupport(gf,v);
      if (vs != pass) continue;
      double prio = irregularityEnergyOnRing(gf, v, adj, vAngle);
      if (vs == CURVE && kv.second.size() > 2) prio += 1000. * std::abs(double(kv.second.size())-2.);
      if (vs == SURFACE && kv.second.size() > 5) prio += 1000. * std::abs(double(kv.second.size())-4.);
      Q.push({prio,v});
    }
    return Q.size();
  }

  int addAffectedVerticesToQueue(
      int pass,
      GFace* gf,
      const std::vector<MElement*>& quads,
      const std::unordered_map<MVertex *, std::vector<MElement *> >& adj,
      const std::unordered_map<MVertex *, double>& vAngle,
      std::priority_queue<std::pair<double,MVertex*>, std::vector<std::pair<double,MVertex*> > >& Q) {
    int count = 0;
    for (MElement* q: quads) {
      /* check if quad still there, necessary because of recursive functions */
      auto itq = std::find(gf->quadrangles.begin(),gf->quadrangles.end(),q);
      if (itq != gf->quadrangles.end()) {
        for (size_t lv = 0; lv < q->getNumVertices(); ++lv) {
          MVertex* v2 = q->getVertex(lv);
          int vs2 = vertexSupport(gf,v2);
          if (vs2 != pass) continue;
          double irreg2 = irregularityEnergyOnRing(gf, v2, adj, vAngle);
          auto it2 = adj.find(v2);
          if (it2 != adj.end()) {
            if (vs2 == CURVE && it2->second.size() > 2) irreg2 += 1000. * std::abs(double(it2->second.size())-2.);
            if (vs2 == SURFACE && it2->second.size() > 5) irreg2 += 1000. * std::abs(double(it2->second.size())-4.);
          }
          if (irreg2 > 0) {
            Q.push({irreg2,v2}); /* add to queue */
            count += 1;
          }
        }
      }
    }
    return count;
  }

  bool transferSeamGEdgesVerticesToGFace(GFace* gf) {
    /* Transfer the vertices from seam GEdge to associated GFace */
    std::unordered_map<MVertex*,MVertex*> old2new;
    for (GEdge* ge: gf->edges()) {
      if (ge->isSeam(gf) && ge->faces().size() == 1 && ge->faces()[0] == gf) {
        for (MVertex* ov: ge->mesh_vertices) {
          SPoint3 p = ov->point();
          double t;
          ov->getParameter(0,t);
          SPoint2 uv = ge->reparamOnFace(gf, t, -1);
          MVertex *nv = new MFaceVertex(p.x(),p.y(),p.z(),gf,uv[0],uv[1]);
          nv->setParameter(0,uv[0]);
          nv->setParameter(1,uv[1]);
          gf->mesh_vertices.push_back(nv);
          old2new[ov] = nv;
          delete ov;
        }
        ge->mesh_vertices.clear();
        for (size_t i = 0; i < ge->lines.size(); ++i) {
          delete ge->lines[i];
        }
        ge->lines.clear();
      }
    }
    if (old2new.size() > 0) {
      for (MElement* f: gf->quadrangles) for (size_t lv = 0; lv < 4; ++lv){ 
        MVertex* v = f->getVertex(lv);
        auto it = old2new.find(v);
        if (it != old2new.end()) {
          f->setVertex(lv, it->second);
        }
      }
    }
    return true;
  }

  bool remeshSmallDefects(GFace* gf) {
    Msg::Debug("- Face %i: remove small quad mesh defects (%li input quads) ...", gf->tag(), gf->quadrangles.size());
    size_t nq_in = gf->quadrangles.size();

    // TODO FROM HERE: detect and remove diamonds

    std::unordered_map<MVertex *, double> vAngle; /* for flat corner on curves */
    std::unordered_map<MVertex *, std::vector<MElement *>> adj;
    for (MQuadrangle* f: gf->quadrangles) {
      for (size_t lv = 0; lv < 4; ++lv) {
        MVertex* v = f->getVertex(lv);
        adj[v].push_back(f);
        int vs = vertexSupport(gf,v);
        if (vs == CORNER || vs == CURVE) {
          MVertex* vPrev = f->getVertex((4+lv-1)%4);
          MVertex* vNext = f->getVertex((lv+1)%4);
          SVector3 pNext = vNext->point();
          SVector3 pPrev = vPrev->point();
          SVector3 pCurr = v->point();
          double agl = angleVectors(pNext-pCurr,pPrev-pCurr);
          vAngle[v] += agl;
        } 
      }
    }

    constexpr bool allowTemporaryDuet = true;

    size_t count[4] = {0,0,0,0};
    for (int pass: {CORNER, CURVE, SURFACE}) {
      if (pass == CORNER) {
        Msg::Debug("- remove defects on corners ...");
      } else if (pass == CURVE) {
        Msg::Debug("- remove defects on curves ...");
      } else if (pass == SURFACE) {
        Msg::Debug("- remove defects on interior ...");
      }

      std::priority_queue<std::pair<double,MVertex*>, std::vector<std::pair<double,MVertex*> > > Q; 

      /* Initialize the priority queue */
      initPriorityQueueWithDefects(pass, gf, adj, vAngle, Q);

      bool remaining = true;
      while (Q.size() > 0) {
        remaining = false;
        MVertex* v = Q.top().second;
        Q.pop();
        size_t num = v->getNum();

        { /* Check if vertex still exists, may have been removed */
          auto it = adj.find(v);
          if (it == adj.end()) continue; 
        }

        int ideal = -1;
        std::vector<MElement*> quads;
        bool toRemesh = remeshableVertexProperties(gf, v, adj, vAngle, ideal, quads);
        if (!toRemesh) continue;
        int vs = vertexSupport(gf, v);

        /* Boundary around quads (includes v if on boundary) */
        std::vector<MVertex*> bnd;
        bool okb = buildBoundary(quads.begin(),quads.end(), bnd);
        if (bnd.size() > 0 && bnd.back() == bnd.front()) bnd.resize(bnd.size()-1);
        if (!okb || bnd.size() < 4) {
          GVertex* gv = dynamic_cast<GVertex*>(v->onWhat());
          if (gv != NULL) {
            Msg::Warning("failed to build boundary loop around vertex %li (on CAD corner %li)", num, gv->tag());
            continue;
          } else {
            Msg::Error("failed to build boundary around vertex %li, bnd.size()=%li", num, bnd.size());
            if (DBG_VIZU) {
              GeoLog::add(SVector3(v->point()),0.,"!failed_one_ring");
              GeoLog::flush();
            }
            continue;
          }
        }

        /* Build the cavity signature (ideal and allowed valences) which is required
         * to find replacements in disk quadrangulations */
        bool cancel = false;
        vector<int> bndIdealValence(bnd.size(),0);
        vector<std::pair<int,int> > bndAllowedValenceRange(bnd.size());
        for (size_t i = 0; i < bnd.size(); ++i) {
          MVertex* bv = bnd[i];
          int bvs = vertexSupport(gf,bv);
          bool onBdr = (bvs == CORNER || bvs == CURVE);
          int bival = 4;
          if (bvs == CORNER) {
            bival = vertexIdealValence(bv,vAngle);
          } else if (bvs == CURVE) {
            bival = 2;
          }
          std::vector<MElement*> bvAdjQuads = adj.at(bv);
          std::vector<MElement*> exterior = difference(bvAdjQuads, quads);
          bndIdealValence[i] = bival - int(exterior.size());
          if (exterior.size() == 0) { /* boundary vertex "inside" the cavity, probably the one to remesh */
            bndAllowedValenceRange[i] = {bival,bival};
          } else if (bvs == CORNER) {
            bndAllowedValenceRange[i] = {1,1}; 
          } else if (bvs == CURVE) {
            if (vs == CORNER) { /* When fixing corners, can degrade curves */
              bndAllowedValenceRange[i] = {1,2};
            } else {
              bndAllowedValenceRange[i] = {1,1};
            }
          } else if (bvs == SURFACE) {
            if (vs == CORNER || vs == CURVE) { /* When fixing corner/curve, can degrade surface */
              if (exterior.size() == 1) { /* warning: may generate quad duets, should check after */
                if (allowTemporaryDuet) {
                  bndAllowedValenceRange[i] = {1,6};  /* allow val 2, 6 and 7 */
                } else {
                  bndAllowedValenceRange[i] = {2,6};
                }
              } else if (exterior.size() == 2) { /* allow 6, 7 */
                bndAllowedValenceRange[i] = {1,5}; 
              } else if (exterior.size() == 3) { /* allow 6, 7 */
                bndAllowedValenceRange[i] = {1,4};
              } else if (exterior.size() == 4) {  /* allow 6, 7 */
                bndAllowedValenceRange[i] = {1,3}; 
              } else if (exterior.size() == 5) { /* allow 6, 7 */
                bndAllowedValenceRange[i] = {1,2}; 
              } else if (exterior.size() >= 6) { /* allow 7+, should minimize */
                bndAllowedValenceRange[i] = {1,1}; 
              }
            } else {
              if (exterior.size() == 1) { /* warning: may generate quad duets, should check after */
                if (allowTemporaryDuet) {
                  bndAllowedValenceRange[i] = {1,4}; 
                } else {
                  bndAllowedValenceRange[i] = {2,4}; 
                }
              } else if (exterior.size() == 2) { /* avoid 6+ */
                bndAllowedValenceRange[i] = {1,3}; 
              } else if (exterior.size() == 3) { /* avoid 6+ */
                bndAllowedValenceRange[i] = {1,2}; 
              } else if (exterior.size() == 4) { /* avoid 6+ */
                bndAllowedValenceRange[i] = {1,1}; 
              } else if (exterior.size() >= 5) { /* bad, will generate 6+ ... */
                bndAllowedValenceRange[i] = {1,1}; 
              }
            }
          } else {
            Msg::Error("case not supported ... onBdr=%i, bvAdjQuads.size()=%li, exterior.size()=%li, ideal val=%i", onBdr, bvAdjQuads.size(), exterior.size(), ideal);
            if (DBG_VIZU) {
              GeoLog::add(SVector3(v->point()),0.,"!cns");
              geolog_closed_curve(bnd, "!cns");
              geolog_elements(quads,"!cns");
              geolog_elements(exterior,"!cns_ext");
              GeoLog::flush();
            }
            cancel = true;
          }
        }
        if (cancel) continue;

        /* Extract vertices inside cavity */
        std::vector<MVertex*> inside;
        verticesStrictlyInsideCavity(gf, quads, bnd, inside);

        /* Get the remeshing with matching disk quadrangulation
         * (GFace is NOT modified by call to remeshFewQuads(), edition should confirmed after) */
        std::vector<MVertex*> newVertices;
        std::vector<bool> vertexIsIrregular;
        std::vector<MElement*> newElements;
        int status = remeshFewQuads(gf, bnd, bndIdealValence, bndAllowedValenceRange, 
            newVertices, vertexIsIrregular, newElements);
        if (status == 0) {
          /* Check if pattern is better (except if duet)*/
          bool duet = (vs == SURFACE && bnd.size() == 4 && quads.size() == 2 && newElements.size() != 2);
          if (!duet) {
            double energy_before = irregularityEnergyOfCavity(gf, bnd, bndIdealValence, bndAllowedValenceRange, inside, quads);
            double energy_after = irregularityEnergyOfCavity(gf, bnd, bndIdealValence, bndAllowedValenceRange, newVertices, newElements);
            if (energy_after >= energy_before) { /* cancel remeshing */
              Msg::Debug("disk quadrangulation rejected because irregularity energy would increase %f -> %f",
                  energy_before, energy_after);
              for (MVertex* nv: newVertices) delete nv;
              for (MElement* nf: newElements) delete nf;
              continue;
            }
          }

          /* Add the new elements to the current GFace */
          for (MVertex* nv: newVertices) gf->mesh_vertices.push_back(nv);
          for (MElement* nf: newElements) {
            gf->quadrangles.push_back(dynamic_cast<MQuadrangle*>(nf));
            addToAdjacencyList(gf, nf, adj);
          }

          if (true && DBG_VIZU) {
            geolog_closed_curve(bnd, "scav_"+std::to_string(bnd.size())+"_"+std::to_string(num));
            geolog_elements(quads,"scav_"+std::to_string(bnd.size())+"_"+std::to_string(num));
            geolog_elements(newElements,"scav_"+std::to_string(bnd.size())+"_"+std::to_string(num)+"_rmsh");
            GeoLog::flush();
          }

          /* Remove old quads and old vertices (strictly inside) */
          for (MElement* f: quads) removeElement(gf, f, adj);
          for (MVertex* v2: inside) removeVertex(v2, adj);

          /* Check if there are duets in bnd */
          bool added_duet = false;
          for (size_t j = 0; j < bnd.size(); ++j) {
            MVertex* v2 = bnd[j];
            if (vertexSupport(gf,v2) == SURFACE && adj.at(v2).size() == 2) {
              /* Duet are high priority stuff ! */
              Q.push({DBL_MAX,v2});
              added_duet = true;
            }
          }

          /* Fill the priority queue */
          addAffectedVerticesToQueue(pass, gf, newElements, adj, vAngle, Q);
          remaining = true;

          if (PARANO && !added_duet) {
            bool valid = slowVerifyMeshIsValid(gf);
            if (!valid) {
              Msg::Error("invalid quad mesh");
              // return false;
            }
            // meshWinslow2d(gf, 10);
          }

          count[pass] += 1;

        } else {
          if (DBG_VIZU) {
            geolog_closed_curve(bnd, "!scav_P"+std::to_string(pass)+"_b"+std::to_string(bnd.size())+"_"+std::to_string(num));
            geolog_elements(quads, "!scav_P"+std::to_string(pass)+"_b"+std::to_string(bnd.size())+"_"+std::to_string(num));
            GeoLog::flush();
          }
          Msg::Warning("failed to remesh around vertex %li (cavity with %li bdr vertices, %li quads)", num,
              bnd.size(), quads.size());
        }
        if (Q.size() == 0 && remaining) { /* try again if there are still defects */
          initPriorityQueueWithDefects(pass, gf, adj, vAngle, Q);
        }
      }
    }

    if (count[CORNER] + count[CURVE] + count[SURFACE] > 0) {
      Msg::Info("- Face %i: remeshed %li corner defects, %li curve defects, %li interior defects (%li -> %li quads)", 
          gf->tag(), count[CORNER], count[CURVE], count[SURFACE], nq_in, gf->quadrangles.size());
    } else {
      return true;
    }

    bool meshok = slowVerifyMeshIsValid(gf);
    if (!meshok) {
      Msg::Error("face %i, mesh no longer valid after removing defects ... (should never happen)", gf->tag());
      // return false;
    }

    Msg::Debug("-- Face %i: winslow smoothing ...", gf->tag());
    meshWinslow2d(gf, 10);

    { /* final check */
      Msg::Debug("-- Face %i: counting remaining defects ...", gf->tag());
      for (auto& kv: adj) {
        MVertex* v = kv.first;
        int vs = vertexSupport(gf,v);
        if (vs == CORNER && kv.second.size() != (size_t) vertexIdealValence(v, vAngle))  {
          Msg::Warning("  - still defect at vertex %li: on corner but %li adj quads",
              v->getNum(), adj[v].size());
          if (DBG_VIZU) GeoLog::add(SVector3(v->point()),double(kv.second.size()),"defect_gf" + std::to_string(gf->tag()) + "_bdr_" + std::to_string(kv.second.size()));
        } else if (vs == CURVE && kv.second.size() != 2) {
          Msg::Warning("  - still defect at vertex %li: on boundary but %li adj quads",
              v->getNum(), adj[v].size());
          if (DBG_VIZU) GeoLog::add(SVector3(v->point()),double(kv.second.size()),"defect_gf" + std::to_string(gf->tag()) + "_bdr_" + std::to_string(kv.second.size()));
        } else if (vs == SURFACE) {
          if (kv.second.size() <= 2 || kv.second.size() > 5) {
            Msg::Warning("  - still defect at vertex %li: inside surface but %li adj quads",
                v->getNum(), adj[v].size());
            if (DBG_VIZU) GeoLog::add(SVector3(v->point()),double(kv.second.size()),"defect_gf" + std::to_string(gf->tag()) + "_int_" + std::to_string(kv.second.size()));
          }
        }
      }
      if (DBG_VIZU) GeoLog::flush();
    }

    return true;
  }

  bool remeshCavityWithGmsh(GFace* gf, FCavity& cav, std::vector<MVertex*>& newSingularities) {
    MeshHalfEdges& M = cav.M;

    /* Inputs for gmsh cavity remesher */
    std::set<MElement*> quads;
    std::vector<MVertex*> bnd;
    bnd.reserve(quads.size());
    std::map<MVertex *, std::vector<MElement *>, MVertexPtrLessThan> adj;

    for (size_t f: cav.quads) {
      MElement* elt = M.faces[f].ptr;
      if (elt == NULL) continue;
      quads.insert(elt);
      for (size_t lv = 0; lv < 4; ++lv) {
        MVertex* v = elt->getVertex(lv);
        adj[v].push_back(elt);
      }
    }

    /* Boundary contour from sides */
    size_t nsides = 0;
    for (size_t i = 0; i < cav.hes.size(); ++i) {
      size_t he = cav.hes[i];
      if (cav.side[i] > nsides) nsides = cav.side[i];
      MVertex* v = M.vertices[M.hedges[he].vertex].ptr;
      if (v == NULL) continue;
      bnd.push_back(v);
    }
    std::reverse(bnd.begin(),bnd.end());
    nsides += 1;

    /* Remesh with gmsh */
    std::map<MVertex*,int, MVertexPtrLessThan> newSings;
    Msg::Info("remeshing cavity with %li quads, %li bdr vertices ...", quads.size(), bnd.size());
    int status = remeshCavity(gf, nsides, quads, bnd, adj, newSings);
    if (status != 1) return false;
    for (const auto& kv: newSings) {
      newSingularities.push_back(kv.first);
    }

    /* Smooth new quads */
    return true;
  }

  bool remeshCavityWithQuadPatterns(GFace* gf, FCavity& cav, std::vector<MVertex*>& newSingularities, SPoint3* center = NULL) {
    MeshHalfEdges& M = cav.M;

    uint8_t smax = 0;
    std::vector<std::vector<MVertex*> > sides;
    for (size_t i0 = 0; i0 < cav.hes.size(); ++i0) {
      size_t i0prev = (i0-1+cav.hes.size())%cav.hes.size();
      smax = std::max(smax,cav.side[i0]);
      if (cav.side[i0] == cav.side[i0prev]) continue;
      for (size_t i = 0; i < cav.hes.size(); ++i) {
        size_t pos = (i0 + i)%cav.hes.size();
        size_t he = cav.hes[pos];
        uint8_t side = cav.side[pos];
        if (side >= sides.size()) sides.resize(side+1);
        MVertex* v1 = M.vertices[M.vertex(he,0)].ptr;
        MVertex* v2 = M.vertices[M.vertex(he,1)].ptr;
        if (sides[side].size() == 0) sides[side].push_back(v1);
        sides[side].push_back(v2);
      }
      break;
    }

    if (sides.size() == 0 && smax < 1) { /* only one side */
      for (size_t i = 0; i < cav.hes.size(); ++i) {
        size_t he = cav.hes[i];
        uint8_t side = cav.side[i];
        if (side >= sides.size()) sides.resize(side+1);
        MVertex* v1 = M.vertices[M.vertex(he,0)].ptr;
        MVertex* v2 = M.vertices[M.vertex(he,1)].ptr;
        if (sides[side].size() == 0) sides[side].push_back(v1);
        sides[side].push_back(v2);
      }
    }

    Msg::Info("remeshing cavity with %li quads, %li sides ...", cav.quads.size(), sides.size());

    std::vector<MElement*> newElements;
    std::vector<MVertex*> newVertices;
    std::vector<size_t> sideSizes(sides.size());
    for (size_t i = 0; i < sideSizes.size(); ++i) sideSizes[i] = sides[i].size();
    std::pair<size_t,int> patternNoAndRot;
    size_t Ncorners = sideSizes.size();
    double irreg;
    bool meshable = patchIsRemeshableWithQuadPattern(Ncorners, sideSizes, patternNoAndRot, irreg);
    if (meshable) {
      std::vector<MQuadrangle*> oldQuads = gf->quadrangles;
      std::vector<MVertex*> oldVertices = gf->mesh_vertices;

      std::vector<bool> vertexIsIrregular;
      int status = remeshPatchWithQuadPattern(gf, sides, patternNoAndRot, newVertices, vertexIsIrregular, newElements, center);
      if (status != 0) {
        Msg::Error("Face %li, failed to remesh with selected quad pattern, weird", gf->tag());
        return 1;
      }

      /* Remove internal vertices and quads */
      vector<MElement*> quads;
      quads.reserve(cav.quads.size());
      for (size_t f: cav.quads) {
        quads.push_back(M.faces[f].ptr);
      }
      vector<MVertex*> bnd;
      buildBoundary(quads.begin(),quads.end(),bnd);
      vector<MVertex*> inside;
      verticesStrictlyInsideCavity(gf, quads, bnd, inside);
      for (MElement* elt: quads) {
        MQuadrangle *q = dynamic_cast<MQuadrangle*>(elt);	      
        if (!q)Msg::Error ("A non quad is present in the list of quad of face %lu",gf->tag());
        gf->quadrangles.erase (std::remove(gf->quadrangles.begin(),gf->quadrangles.end(),q),gf->quadrangles.end());
        delete q;
      }
      for (MVertex* v: inside) {
        auto it = std::find(gf->mesh_vertices.begin(),gf->mesh_vertices.end(),v);
        if (it != gf->mesh_vertices.end()) gf->mesh_vertices.erase(it);
        delete v;
      }

      /* Check orientation of the remeshed cavity compared to input,
       * and invert if required */
      /* - oriented boundary */
      unordered_set<si2,si2hash> oedges;
      for (size_t i = 0; i < bnd.size()-1; ++i) {
        oedges.insert({bnd[i]->getNum(),bnd[i+1]->getNum()});
      }
      bool found = false;
      bool orientation_ok = true;
      for (MElement* f: newElements) {
        for (size_t le = 0; le < 4; ++le) {
          size_t v1 = f->getVertex(le)->getNum();
          size_t v2 = f->getVertex((le+1)%4)->getNum();
          si2 vPair = {v1,v2};
          auto it = oedges.find(vPair);
          if (it != oedges.end()) {
            found = true;
            orientation_ok = true;
            break;
          }
          si2 vPairInv = {v2,v1};
          auto it2 = oedges.find(vPairInv);
          if (it2 != oedges.end()) {
            found = true;
            orientation_ok = false;
            break;
          }
        }
        if (found) break;
      }
      if (!found) {
        Msg::Error("common edge not found ! cannot check orientation");
      }
      if (!orientation_ok) { /* Invert quads ! */
        for (MElement* f: newElements) {
          MVertex* v1 = f->getVertex(1);
          MVertex* v3 = f->getVertex(3);
          f->setVertex(1,v3);
          f->setVertex(3,v1);
        }
      }

      /* Update singularities */
      vector<MVertex*> ivert;
      for (size_t i = 0; i < newVertices.size(); ++i) if (vertexIsIrregular[i]) {
        ivert.push_back(newVertices[i]);
      }
      if (ivert.size() == 1) {
        if (Ncorners == 3) {
          newSingularities.push_back(ivert[0]);
        } else if (Ncorners == 5) {
          newSingularities.push_back(ivert[0]);
        }
      }
    } 

    geolog_elements(newElements, "cavr4_after");
    if (newElements.size() > 0) {
      vector<MQuadrangle*> quadForWinslow(newElements.size());
      for (size_t i = 0; i < newElements.size(); ++i) {
        quadForWinslow[i] = dynamic_cast<MQuadrangle*>(newElements[i]);

      }
      meshWinslow2d(gf, quadForWinslow, newVertices, 10);
    }
    geolog_elements(newElements, "cavr4_after_smooth");

    return true;
  }


  int remeshCavitiesAroundSingularities(GFace* gf, std::vector<MVertex*>& singularVertices) 
  {
    Msg::Info("- Face %i: remeshCavitiesAroundSingularities ...", gf->tag());

    using std::priority_queue;
    using std::pair;

    MeshHalfEdges M;
    vector<size_t> singularities;
    vector<size_t> irregularNodes;

    size_t count = 0;
    bool inProgress = true;
    while (inProgress) {
      inProgress = false;

      /* singularNodes is the list of singularities (irregular vertices to keep) in the 
       * GFace structure, the values are the vertex 'num' */
      int st = createMeshHalfEdges(gf, M, singularVertices);
      if (st != 0) {
        Msg::Error("failed to generate half edge datastructure for face with tag %i", gf->tag());
        return st;
      }

      Gardener G(M);

      /* Look for the best singularity around which to grow cavity ... */
      singularities.clear();
      irregularNodes.clear();
      for (size_t v = 0; v < M.vertices.size(); ++v) {
        if (M.vertices[v].isSingularity) {
          singularities.push_back(v);
        } else {
          bool onBdr;
          int val = M.vertexFaceValence(v, onBdr);
          if (!onBdr && val != 4) irregularNodes.push_back(v);
        }
      }
      vector<double> priority(singularities.size(),0.);
      for (size_t i = 0; i < irregularNodes.size(); ++i) {
        size_t iv = irregularNodes[i];
        SVector3 ip = M.vertices[iv].p;
        for (size_t j = 0; j < singularities.size(); ++j) {
          SVector3 sp = M.vertices[singularities[j]].p;
          priority[j] += 1./(ip-sp).norm();
        }
      }
      std::vector<std::pair<double,size_t> > prio_sing(singularities.size());
      for (size_t i = 0; i < singularities.size(); ++i) prio_sing[i] = {priority[i],singularities[i]};
      std::sort(prio_sing.begin(),prio_sing.end());
      std::reverse(prio_sing.begin(),prio_sing.end());

      /* Try the cavities */
      for (size_t i = 0; i < prio_sing.size(); ++i) {
        size_t v = prio_sing[i].second;
        /* Init */
        FCavity fcav(M);
        vector<size_t> quads;
        M.vertexFaces(v, quads);
        bool ok = fcav.init(quads);
        if (!ok) {
          Msg::Error("failed to init cavity");
          continue;
        }
        // geolog_fcavity(fcav, "fcav"+std::to_string(v)+"_init");

        /* Build a cavity around singularity i */
        G.setCavity(fcav);

        bool okg = G.growMaximal();
        if (!okg) continue;

        {
          int ns = fcav.updateSides();
          geolog_fcavity(fcav, "cavr"+std::to_string(ns)+"_"+std::to_string(v)+"_before");
        }

        /* Remesh the cavity */
        std::vector<MVertex*> newSingularities;
        size_t nq = gf->quadrangles.size();
        // bool okr = remeshCavityWithGmsh(gf,fcav,newSingularities);
        SPoint3 center = M.vertices[v].ptr->point();
        bool okr = remeshCavityWithQuadPatterns(gf,fcav,newSingularities,&center);
        if (okr) { /* then cavity and M are no longer valid, restart */
          size_t nq2 = gf->quadrangles.size();
          if (nq2 == nq)  {
            Msg::Warning("same number of quads in GFace after remeshing... weird");
          }

          /* update list of singular nodes */
          auto itv = std::find(singularVertices.begin(),singularVertices.end(),M.vertices[v].ptr);
          if (itv != singularVertices.end()) {
            singularVertices.erase(itv);
          }
          for (MVertex* v2: newSingularities) {
            singularVertices.push_back(v2);
          }
          inProgress = true;
          count += 1;
          break;
        } else {
          Msg::Info("-> failed to remesh cavity");
          geolog_fcavity(fcav, "failed_cav"+std::to_string(v));
        }
      }
    }

    if (count > 0) {
      Msg::Info("winslow smoothing of the face (%li quads) ...", gf->quadrangles.size());
      meshWinslow2d(gf, 10);
    }

    return 0;
  }

  int remeshQuadrilateralPatches(GFace* gf, std::vector<MVertex*>& singularVertices) {
    Msg::Info("remeshQuadrilateralPatches ...");

    using std::priority_queue;
    using std::pair;

    MeshHalfEdges M;
    vector<size_t> pairs35;
    vector<size_t> singularities;
    vector<size_t> irregularNodes;

    size_t count = 0;
    bool inProgress = true;
    while (inProgress) {
      inProgress = false;

      /* singularNodes is the list of singularities (irregular vertices to keep) in the 
       * GFace structure, the values are the vertex 'num' */
      int st = createMeshHalfEdges(gf, M, singularVertices);
      if (st != 0) {
        Msg::Error("failed to generate half edge datastructure for face with tag %i", gf->tag());
        return st;
      }

      Gardener G(M);

      { /* Collect singularities and irregular vertices */
        singularities.clear();
        irregularNodes.clear();
        for (size_t v = 0; v < M.vertices.size(); ++v) {
          if (M.vertices[v].isSingularity) {
            singularities.push_back(v);
          } else if (G.vOnBoundary[v] && G.valence[v] != 2) {
            MVertex* vp = M.vertices[v].ptr;
            GVertex* gv = vp->onWhat()->cast2Vertex();
            if (gv != nullptr)  continue; /* ignore corners */
            irregularNodes.push_back(v);
          } else if (!G.vOnBoundary[v] && G.valence[v] != 4) {
            irregularNodes.push_back(v);
          }
        }
      }

      { /* Collect quads with pairs of 3-5 (or equivalent on boundary) */
        pairs35.clear();
        vector<size_t> vertIn35;
        vector<size_t> fvert;
        const array<int,4> indices35 = {-1,0,1,0};
        array<int,4> quadIndices = {0,0,0,0};
        for (size_t f = 0; f < M.faces.size(); ++f) {
          size_t n = M.face_vertices(f,fvert);
          if (n != 4) {
            Msg::Error("face %li is not a quad ? %li vertices", f, n);
            return false;
          }
          for (size_t lv = 0; lv < n; ++lv) {
            quadIndices[lv] = (G.vOnBoundary[fvert[lv]]) ?  2 - G.valence[fvert[lv]] : 4 - G.valence[fvert[lv]];
          }
          quadIndices = rotateCanonical(quadIndices);
          if (quadIndices == indices35) {
            pairs35.push_back(f);
            append(vertIn35,fvert);
          }
        }
        /* remove irregular nodes of 3-5 from list */
        irregularNodes = difference(irregularNodes, vertIn35); 
      }

      /* Look for the best 3-5 pair around which to grow cavity ... */
      // priority = distance contrib from other 3-5 and TODO repulson from singularities
      vector<double> priority(pairs35.size(),0.);
      for (size_t i = 0; i < pairs35.size(); ++i) {
        size_t f = pairs35[i];
        SVector3 pos = M.vertices[M.hedges[M.faces[f].he].vertex].p;
        for (size_t j = 0; j < pairs35.size(); ++j) if (i != j) {
          SVector3 pos2 = M.vertices[M.hedges[M.faces[pairs35[j]].he].vertex].p;
          double dist = (pos-pos2).norm();
          if (dist != 0.) priority[i] += 1./dist;
        }
      }
      std::vector<std::pair<double,std::vector<size_t> > > prio_quads(priority.size());
      for (size_t i = 0; i < pairs35.size(); ++i) prio_quads[i] = {priority[i],{pairs35[i]}};
      std::sort(prio_quads.begin(),prio_quads.end());
      std::reverse(prio_quads.begin(),prio_quads.end());

      /* Add quads around isolated irregular vertices */
      for (size_t vi: irregularNodes) if (!G.vOnBoundary[vi]){
        vector<size_t> quads;
        M.vertexFaces(vi, quads);
        prio_quads.push_back({0.,quads});
      }

      /* Try the cavities */
      for (size_t i = 0; i < prio_quads.size(); ++i) {
        /* Init */
        FCavity fcav(M);
        const vector<size_t>& quads = prio_quads[i].second;
        bool ok = fcav.init(quads);
        if (!ok) {
          Msg::Error("failed to init cavity");
          continue;
        }
        // geolog_fcavity(fcav, "cavr4_"+std::to_string(f)+"_init");

        /* Build a cavity around singularity i */
        G.setCavity(fcav);

        bool okg = G.growMaximal();
        if (!okg) continue;

        geolog_fcavity(fcav, "cavr4_"+std::to_string(quads[0])+"_before");

        /* Remesh the cavity */
        std::vector<MVertex*> newSingularities;
        size_t nq = gf->quadrangles.size();

        SPoint3 center = M.faces[quads[0]].ptr->getVertex(0)->point();
        bool okr = remeshCavityWithQuadPatterns(gf,fcav,newSingularities,&center);
        // bool okr = remeshCavityWithGmsh(gf,fcav,newSingularities);
        if (okr) { /* then cavity and M are no longer valid, restart */
          size_t nq2 = gf->quadrangles.size();
          if (nq2 == nq)  {
            Msg::Warning("same number of quads in GFace after remeshing... weird");
          } else {
            inProgress = true;
          }
          count += 1;
          break;
        } else {
          Msg::Info("-> failed to remesh cavity");
          geolog_fcavity(fcav, "failed_cav_q"+std::to_string(quads[0]));
        }
      }
    }

    if (count > 0) {
      Msg::Info("winslow smoothing of the face (%li quads) ...", gf->quadrangles.size());
      meshWinslow2d(gf, 100);
    }

    return 0;
  }

  int meshSimpleFaceWithPattern(GFace* gf, const GFaceInfo& info) {
    vector<GEdge *> const &edges = gf->edges();
    vector<int> orientations(gf->edgeOrientations());
    unordered_map<GEdge*,int> edge_orientation;
    for (size_t i = 0; i < edges.size(); ++i) edge_orientation[edges[i]] = orientations[i];

    unordered_map<GVertex*,vector<GEdge*> > v2e;
    for (GEdge* ge: edges) for (GVertex* gv: ge->vertices()) v2e[gv].push_back(ge);

    std::set<GVertex*> corners = info.bdrValVertices[1];
    bool disk = (corners.size() == 0 && info.bdrValVertices[2].size() > 0);
    // TODO check geometric convexity of the disk ... should not apply on
    //      faces with lot of convex and concave curves
    
    // if (!disk) return 1; // DEBUGING TODO REMOVE

    /* Sort CAD edges in sides */
    vector<vector<GEdge*> > sides;
    for (GEdge* e0: edges) {
      GVertex* v1 = e0->vertices()[0];
      GVertex* v2 = e0->vertices()[1];
      bool v1IsCorner = (corners.find(v1) != corners.end());
      if (!disk && !v1IsCorner) continue;
      if (disk) sides.resize(1);

      GVertex* v = v1;
      GEdge* e = e0;
      do {
        bool vIsCorner = (corners.find(v) != corners.end());
        if (vIsCorner) {
          sides.resize(sides.size()+1);
        }

        sides.back().push_back(e);

        v1 = e->vertices()[0];
        v2 = e->vertices()[1];
        GVertex* v_next = (v2 != v) ? v2 : v1;
        GEdge* e_next = NULL;
        if (v2e[v_next].size() == 2) {
          e_next = (v2e[v_next][0] != e) ? v2e[v_next][0] : v2e[v_next][1];
        } else {
          Msg::Warning("non manifold edge loop around face %li, cancel simple quad mesh", gf->tag());
          return false;
        }

        e = e_next;
        v = v_next;
      } while (e != e0);
      break;
    }

    if (sides.size() == 0) {
      Msg::Debug("face %i (%li edges), failed to build sides",gf->tag(),edges.size());
      return -1;
    }

    /* Collect mesh vertices */
    vector<vector<MVertex*> > sideVertices(sides.size());
    for (size_t i = 0; i < sides.size(); ++i) {
      for (size_t j = 0; j < sides[i].size(); ++j) {
        GEdge* ge = sides[i][j];
        GVertex* v1 = ge->vertices()[0];
        GVertex* v2 = ge->vertices()[1];
        size_t n = ge->mesh_vertices.size();
        if (edge_orientation[ge] == 1) {
          if (j == 0) { /* First GEdge */
            sideVertices[i].push_back(v1->mesh_vertices.at(0));
          } 
          for (size_t k = 0; k < n; ++k) {
            sideVertices[i].push_back(ge->mesh_vertices[k]);
          }
          sideVertices[i].push_back(v2->mesh_vertices.at(0));
        } else if (edge_orientation[ge] == -1) {
          if (j == 0) { /* First GEdge */
            sideVertices[i].push_back(v2->mesh_vertices.at(0));
          } 
          for (size_t k = 0; k < n; ++k) {
            sideVertices[i].push_back(ge->mesh_vertices[n-1-k]);
          }
          sideVertices[i].push_back(v1->mesh_vertices.at(0));
        } else {
          Msg::Error("weird orientation %i for edge %i ?", edge_orientation[ge], ge->tag());
        }
      }
    }

    /* When there is just one side, two possible cases:
     * - disk (all bdr vertices are quad-valence 2)
     * - face with only one convex corner
     */
    if (sides.size() == 1) {
      if (sides[0].size() < 1) {
        Msg::Warning("not enough vertices on face loop for quad meshing (%li vert)", sides[0].size());
        return 1;
      }
      if (disk) {
        if (sides[0].front() == sides[0].back()) {
          sides[0].pop_back();
        }
      } else {
        if (sides[0].front() == sides[0].back()) {
          sides[0].pop_back();
        }
      }
    }


    // if (sideVertices.size() > 2) return false; // DEBUG TODO

    // DBG("---------",gf->tag());
    // DBG(sideVertices.size());
    std::vector<size_t> sideSizes(sideVertices.size());
    for (size_t i = 0; i < sideSizes.size(); ++i) sideSizes[i] = sideVertices[i].size();
    /* Mesh with quad pattern */
    std::pair<size_t,int> patternNoAndRot;
    size_t Ncorners = disk ? 0 : sideSizes.size();
    double irreg;
    bool meshable = patchIsRemeshableWithQuadPattern(Ncorners, sideSizes, patternNoAndRot, irreg);
    if (meshable) {
      std::vector<MQuadrangle*> oldQuads = gf->quadrangles;
      std::vector<MVertex*> oldVertices = gf->mesh_vertices;

      std::vector<MVertex*> newVertices;
      std::vector<bool> vertexIsIrregular;
      std::vector<MElement*> newElements;
      int status = remeshPatchWithQuadPattern(gf, sideVertices, patternNoAndRot, newVertices, vertexIsIrregular, newElements);
      if (status == 0) {
        /* Remove old mesh elements */
        gf->quadrangles = difference(gf->quadrangles, oldQuads);
        gf->mesh_vertices = difference(gf->mesh_vertices, oldVertices);
        meshWinslow2d(gf, 10);
        gf->meshStatistics.status = GFace::DONE;
        return 0;
      } else {
        Msg::Error("Face %li, failed to remesh with selected quad pattern, weird", gf->tag());
        return 1;
      }
    } 
    return 1;
  }

}

using namespace QSQ;

int setQuadCoherentCurveTransfiniteConstraints(const std::vector<GFace*>& faces, const std::map<GFace*,GFaceInfo>& faceInfo) 
{
  Msg::Debug("mesh %li faces with quad integer constraint solver ...", faces.size());

  return 0;
}

int generateInitialTriangulation(GModel* gm) {
  std::vector<GFace*> faces = model_faces(gm);
  // for (GFace* gf: faces) gf->setMeshingAlgo(ALGO_2D_FRONTAL);

  /* Frontal Delaunay triangulator with sufficient points on
   * curves to capture curvatures in cross field */
  // CTX::instance()->mesh.minCurvPoints = 10;
  // CTX::instance()->mesh.minCircPoints = 40;
  // CTX::instance()->mesh.lcFromCurvature = 1;
  // CTX::instance()->mesh.minElementsPerTwoPi = 40;
  CTX::instance()->mesh.algo2d = ALGO_2D_FRONTAL;
  CTX::instance()->lock = 0;
  GenerateMesh(gm, 1);
  GenerateMesh(gm, 2);
  CTX::instance()->lock = 1;
  CTX::instance()->mesh.algo2d = ALGO_2D_QUAD_QUASI_STRUCT;
  CTX::instance()->mesh.lcFromCurvature = 0;
  CTX::instance()->mesh.minCurvPoints = 0;
  CTX::instance()->mesh.minCircPoints = 0;

  // for (GFace* gf: faces) gf->setMeshingAlgo(ALGO_2D_QUAD_QUASI_STRUCT);
  return 0;
}

size_t numberOfTriangles(GModel* gm) {
  size_t N = 0;
  for(GModel::fiter it = gm->firstFace(); it != gm->lastFace(); ++it) {
    N += (*it)->triangles.size();
  }
  return N;
}

int computeScaledCrossField(GModel* gm, std::vector<std::array<double,5> >& singularities) {
  int viewTag = -1;
  int targetNumberOfQuads = 0.5*numberOfTriangles(gm);
  // FIXME: scaling the number of quads break something ! the number of points
  //        on curves are not scaled with the background field
  // Msg::Warning("target number of quads not divided by 4 because of incoherencies between curves/surfaces mesh");
  targetNumberOfQuads *= 0.25; /* because of future midpoint subdivision */
  bool disableConformalScaling = true; /* On some corners, H tends to infinity and may destroy the size map */
  int nbDiffusionLevels = 7;
  double thresholdNormConvergence = 1.e-2;
  int nbBoundaryExtensionLayer = 1;
  std::string name = "scaled_cross_field";
  {
    PView* view_s = PView::getViewByName(name);
    if (view_s) {
      viewTag = view_s->getTag();
    }
  }
  singularities.clear();
  int verbosity = 0;
  int st = computeScaledCrossFieldView(gm, viewTag, targetNumberOfQuads, 
      nbDiffusionLevels, thresholdNormConvergence, nbBoundaryExtensionLayer, 
      name, verbosity, &singularities, disableConformalScaling);
  double acute = 30.;
  addSingularitiesAtAcuteCorners(model_faces(gm), acute, singularities);
  if (st == 0) {
    gm->getFields()->setBackgroundMesh(viewTag);
    // gm->getFields()->initialize(); // required ?
    // PView* view_s = PView::getViewByName(name);
    // if (view_s) view_s->getOptions()->visible = 0;
  }
  return st;
}

int computeQuadCurveMeshConstraints(GModel* gm) {
  Msg::Warning("-- curve constraints not yet implemented ...");
  return 1;
}

int generateCurve1DMeshes(GModel* gm) {
  /* Disable clscale because we have a sizemap 
   * that contains the scaling */
  double clscale = CTX::instance()->mesh.lcFactor;
  CTX::instance()->mesh.lcFactor = 1.;

  computeQuadCurveMeshConstraints(gm);

  /* Remove triangulations */
  std::for_each(gm->firstFace(), gm->lastFace(), deMeshGFace());

  std::vector<GEdge*> edges = model_edges(gm);
#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic)
#endif
  for (GEdge* ge: edges) {
    ge->mesh(false);
  }

  CTX::instance()->mesh.lcFactor = clscale;
  return 0;
}

int generatePatternBasedQuadMeshesOnSimpleFaces(GModel* gm, std::map<GFace*, GFaceInfo>& faceInfo) {
  CTX::instance()->lock = 0;
  std::vector<GFace*> faces = model_faces(gm);
  for (GFace* gf: faces) {
    GFaceInfo& info = faceInfo[gf];
    /* Check if convex topological disk */
    if (info.chi == 1 && info.bdrValVertices[1].size() >= 0 
        && info.bdrValVertices[3].size() == 0 && info.bdrValVertices[4].size() == 0) {
      /* Mesh with pattern */
      int status = meshSimpleFaceWithPattern(gf, info);
      if (status == 0) {
        gf->meshStatistics.status = GFace::DONE;
        Msg::Info("- Face %i: simple (topo. disk, %i convex corners), built quad meshes (%li elements) from simple pattern",
            gf->tag(), info.bdrValVertices[1].size(), gf->quadrangles.size());
      }
    }
  }

  return 0;
}

int generateUnstructuredQuadMeshes(GModel* gm, std::map<GFace*, GFaceInfo>& faceInfo) {
  /* Disable clscale because we have a sizemap 
   * that contains the scaling */
  double clscale = CTX::instance()->mesh.lcFactor;
  CTX::instance()->mesh.lcFactor = 1.;


  /* Generate quad dominant mesh */

  std::vector<GFace*> faces = model_faces(gm);

  CTX::instance()->mesh.algo2d = ALGO_2D_PACK_PRLGRMS;
  CTX::instance()->lock = 0;

#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic)
#endif
  for (GFace* gf: faces) {
    if (gf->meshStatistics.status == GFace::PENDING) {
      gf->setMeshingAlgo(ALGO_2D_PACK_PRLGRMS);
      gf->mesh(true);
      /* set to pending to enable future topological improvement */
      gf->meshStatistics.status = GFace::PENDING; 
      gf->setMeshingAlgo(ALGO_2D_QUAD_QUASI_STRUCT);
    }
  }

  CTX::instance()->lock = 1;
  CTX::instance()->mesh.algo2d = ALGO_2D_QUAD_QUASI_STRUCT;

  for (GFace* gf: faces) if (gf->quadrangles.size() == 0 && gf->triangles.size() == 0) {
    Msg::Error("face %i: no quads and no triangles, abort", gf->tag());
    gf->meshStatistics.status = GFace::FAILED; 
    continue;
    // CTX::instance()->mesh.lcFactor = clscale;
    // return -1;
  }

  bool secondOrderLinear = false; /* which value to use ? */
  RefineMesh(gm, secondOrderLinear, true, false);

  CTX::instance()->mesh.lcFactor = clscale;
  return 0;
}

using si4 = std::array<size_t,4>;
using i4 = std::array<int,4>;
bool quad_config(const MeshHalfEdges& M, const std::vector<int>& valence,
    size_t f, si4& vert, si4& hedges, i4& qVals)
{
  size_t he = M.faces[f].he;
  if (he == NO_ID) {
    Msg::Error("quad_config | face=%li, he=%li", f, he);
    return false;
  }
  size_t count = 0;
  do {
    size_t v = M.hedges[he].vertex;
    vert[count] = v;
    hedges[count] = he;
    he = M.next(he);
    count += 1;
    if (he == NO_ID || count > 4) {
      Msg::Error("quad_config | face=%li, count=%li, he=%li", f, count, he);
      return false;
    }
  } while (he != M.faces[f].he);
  int minVal = 9999;
  size_t minValLv = 0;
  for (size_t lv = 0; lv < 4; ++lv) {
    qVals[lv] = valence[vert[lv]];
    if (qVals[lv] < minVal) {
      minVal = qVals[lv];
      minValLv = lv;
    }
  }
  if (minVal == 0) {
    Msg::Error("quad_config | face=%li, minVal is 0", f);
    return false;
  }
  if (minValLv != 0) {
    /* Shift vertices to get minimum valence in first */
    std::rotate(vert.begin(), vert.begin() + minValLv, vert.end());
    std::rotate(hedges.begin(), hedges.begin() + minValLv, hedges.end());
    std::rotate(qVals.begin(), qVals.begin() + minValLv, qVals.end());
  }

  return true;
}

int move35pairsToSingularities(MeshHalfEdges& M) {
  vector<int> valence(M.vertices.size(),0);
  vector<bool> vOnBdr(M.vertices.size(),false);
  for (size_t i = 0; i < M.vertices.size(); ++i) {
    bool onBdr = false;
    valence[i] = M.vertexFaceValence(i,onBdr);
    if (onBdr) vOnBdr[i] = true;
  }

  for (size_t f = 0; f < M.faces.size();++f) {
    si4 vert;
    si4 hedges;
    i4 qVals;
    bool ok = quad_config(M, valence, f, vert, hedges, qVals);
    if (!ok) {
      Msg::Error("invalid vertices in quad");
      continue;
    }
    i4 pair35 = {3,4,5,4};
    if (qVals == pair35) {
      vector<vec3> pts = {
        M.vertices[vert[0]].p,
        M.vertices[vert[1]].p,
        M.vertices[vert[2]].p,
        M.vertices[vert[3]].p};
      vector<double> values(pts.size(),0.);
      GeoLog::add(pts,values,"pair35");
    }
  }
  GeoLog::flush();

  return 0;
}

int improveQuadMeshOfFace(GFace* gf, vector<MVertex*>& singularVertices) {
  // TODO a loop ?
  // TODO: better cavity construction...
  //       graph + XOR on chords ?
  remeshCavitiesAroundSingularities(gf, singularVertices);
  remeshQuadrilateralPatches(gf, singularVertices);
  // remeshCavitiesAroundSingularities(gf, singularNodes);
  // remeshQuadrilateralPatches(gf, singularNodes);
  return 0;
}

int verifyCompatibilityCrossFieldEuler(const GFace* gf, int sum3m5, size_t nSingVal3, size_t nSingVal5) {
  /* Check compatibility between Euler-based topological relation
   * and cross field singularities */
  int cfSum3m5 = int(nSingVal3) - int(nSingVal5);
  if (cfSum3m5 == sum3m5) {
    Msg::Debug("- Face %i | face topology and cross field are compatible, n_val3 - n_val5 = %i - %i = %i", 
        gf->tag(), nSingVal3, nSingVal5, cfSum3m5);
  } else {
    Msg::Warning("- Face %i | face topology and cross field are NOT compatible, crossfield n_val5 - n_val3 = %i - %i = %i but geometry: %i", 
        gf->tag(), nSingVal3, nSingVal5, cfSum3m5, sum3m5);
    return -1;
  }
  return 0;
}

int singularVerticesFromFloatingSingularities(
    const GFace* gf,
    const std::vector<std::array<double,5> >& singularities,
    std::vector<MVertex*>& singularVertices,
    size_t& nSingVal3, size_t& nSingVal5) {
  singularVertices.clear();

  /* Extract sings in current face, from singularity list */
  vector<array<double,3>> cfSingVal3;
  vector<array<double,3>> cfSingVal5;
  for (size_t k = 0; k < singularities.size(); ++k) {
    int faceTag = int(singularities[k][4]);
    if (faceTag != gf->tag()) continue;
    double index = singularities[k][3];
    SVector3 p(singularities[k][0],singularities[k][1],singularities[k][2]);
    if (index == 1.) {
      cfSingVal3.push_back(p);
    } else if (index == -1.) {
      cfSingVal5.push_back(p);
    } else {
      Msg::Warning("singularity with index %f not supported", index);
    }
  }
  nSingVal3 = cfSingVal3.size();
  nSingVal5 = cfSingVal5.size();

  /* Extract irregular vertices in face */
  vector<MVertex*> nodeVal3;
  vector<MVertex*> nodeVal5;
  {
    unordered_map<MVertex*, size_t> valence;
    for (MQuadrangle* q: gf->quadrangles) {
      for (size_t le = 0; le < 4; ++le) {
        MVertex* v = q->getVertex(le);
        if (v->onWhat()->cast2Face() != NULL) {
          valence[v] += 1;
        }
      }
    }
    for (const auto& kv: valence) if (kv.second != 4) {
      if (kv.second == 3) {
        nodeVal3.push_back(kv.first);
      } else if (kv.second == 5) {
        nodeVal5.push_back(kv.first);
      }
    }
  }

  /* Assign valence 3 */
  for (size_t i = 0; i < cfSingVal3.size(); ++i) {
    SVector3 p = {cfSingVal3[i][0],cfSingVal3[i][1],cfSingVal3[i][2]};
    double dmin = DBL_MAX;
    MVertex* best = NULL;
    for (size_t j = 0; j < nodeVal3.size(); ++j) {
      SVector3 p2 = nodeVal3[j]->point();
      double dist = nodeVal3[j]->point().distance(p.point());
      if (dist < dmin) {
        dmin = dist;
        best = nodeVal3[j];
      }
    }
    if (best != NULL) {
      singularVertices.push_back(best);
    } else {
      Msg::Warning("- Face %i, singular node %i, failed to assign to val 3 irregular vertex", gf->tag(), i);
      // GeoLog::add(p,3.,"dbg_p"+std::to_string(gf->tag())+"_remaining_sing");
    }
  }
  /* Assign valence 5 */
  for (size_t i = 0; i < cfSingVal5.size(); ++i) {
    SVector3 p = {cfSingVal5[i][0],cfSingVal5[i][1],cfSingVal5[i][2]};
    double dmin = DBL_MAX;
    MVertex* best = NULL;
    for (size_t j = 0; j < nodeVal5.size(); ++j) {
      SVector3 p2 = nodeVal5[j]->point();
      double dist = nodeVal5[j]->point().distance(p.point());
      if (dist < dmin) {
        dmin = dist;
        best = nodeVal5[j];
      }
    }
    if (best != NULL) {
      singularVertices.push_back(best);
    } else {
      Msg::Warning("- Face %i, singular node %i, failed to assign to val 5 irregular vertex", gf->tag(), i);
      // GeoLog::add(p,5.,"dbg_p"+std::to_string(gf->tag())+"_remaining_sing");
    }
  }

  return 0;
}

int improveQuadMeshTopology(GModel* gm, const std::vector<std::array<double,5> >& singularities,
    std::map<GFace*, GFaceInfo>& faceInfo) {
  vector<GFace*> faces = model_faces(gm);

  /* Improve local defects (valence 6+, valence 3+ on curves, etc)
   * by checking all possible local remeshing in big list of
   * disk quadrangulations */
#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic)
#endif
  for (size_t i = 0; i < faces.size(); ++i) {
    GFace* gf = faces[i];
    transferSeamGEdgesVerticesToGFace(gf);

    if (gf->meshStatistics.status == GFace::PENDING) {
      remeshSmallDefects(gf);
    }
  }
  printPatternUsage();

  /* Improve quad meshes with larger operators (cavity remeshing) */

// #if defined(_OPENMP)
// #pragma omp parallel for schedule(dynamic)
// #endif
  for (size_t i = 0; i < faces.size(); ++i) {
    GFace* gf = faces[i];
    if (gf->meshStatistics.status == GFace::PENDING) {
      size_t nSingVal3, nSingVal5;
      vector<MVertex*> singularVertices;
      singularVerticesFromFloatingSingularities(gf, singularities, singularVertices, nSingVal3, nSingVal5);
      verifyCompatibilityCrossFieldEuler(gf, faceInfo.at(gf).intSumVal3mVal5, nSingVal3, nSingVal5);
      int st = improveQuadMeshOfFace(gf,singularVertices);
      if (st != 0) {
        Msg::Error("failed to improve quad mesh of face with tag %i", gf->tag());
        continue;
      }
    }
  }
  return 0;
}

int Mesh2DWithQuadQuasiStructured(GModel* gm)
{
  if (CTX::instance()->mesh.algo2d != ALGO_2D_QUAD_QUASI_STRUCT) {
    return 1;
  }
  std::vector<GFace*> faces = model_faces(gm);

  Msg::Info("Generate quasi-structured all-quadrilateral mesh ...");

  Msg::Info("[Step 1] Generate initial triangulation ...");
  int s1 = generateInitialTriangulation(gm);
  if (s1 != 0) {
    Msg::Error("failed to generate initial triangulation, abort");
    return s1;
  }

  /* Use the triangulation to compute CAD face information 
   * (nb corners of each valence, euler characteristic, etc) */
  std::map<GFace*, GFaceInfo> faceInfo;
  for (GFace* gf: faces) {
    GFaceInfo info;
    bool ok = fillGFaceInfo(gf, info);
    if (ok) faceInfo[gf] = info;
  }

  Msg::Info("[Step 2] Generate scaled cross field ...");
  std::vector<std::array<double,5> > singularities;
  int s2 = computeScaledCrossField(gm, singularities);
  if (s2 != 0) {
    Msg::Error("failed to compute scaled cross field, abort");
    return s2;
  }

  Msg::Info("[Step 3] Generate curve 1D meshes ...");
  int s3 = generateCurve1DMeshes(gm);
  if (s3 != 0) {
    Msg::Warning("failed to generate curve 1D meshes, abort");
    return s3;
  }

  Msg::Info("[Step 4] Generate unstructured quad meshes ...");
  int s4 = generateUnstructuredQuadMeshes(gm, faceInfo);
  if (s4 != 0) {
    Msg::Warning("failed to generate 2D unstructured quad meshes, abort");
    return s4;
  }

  bool SHOW_ONLY_PATTERN_MESHING = false;

  /* After Step 4 because the midpoint subdivision helps */
  Msg::Info("[Step 5] Generate pattern-based quad meshes in simple faces ...");
  int s5 = generatePatternBasedQuadMeshesOnSimpleFaces(gm, faceInfo);
  if (s5 != 0) {
    Msg::Warning("failed to generate pattern-based quad meshes, abort");
    return s5;
  }

  /* For visu */
  if (SHOW_ONLY_PATTERN_MESHING) {
    for (GFace* gf: faces) {
      if (gf->meshStatistics.status != GFace::DONE) {
        gf->quadrangles.clear();
        gf->mesh_vertices.clear();
      }
    }
    return 0;
  }

  Msg::Info("[Step 6] Improve topology of quad mesh ...");
  int s6 = improveQuadMeshTopology(gm, singularities, faceInfo);
  if (s6 != 0) {
    Msg::Warning("failed to improve quad mesh topology, continue");
  }

  return 0;
}
