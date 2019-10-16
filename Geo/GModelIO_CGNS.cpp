// Gmsh - Copyright (C) 1997-2019 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#include <string>
#include <map>
#include <utility>
#include "GmshConfig.h"
#include "GmshMessage.h"
#include "GModel.h"
#include "partitionRegion.h"
#include "partitionFace.h"
#include "partitionEdge.h"
#include "partitionVertex.h"
#include "CGNSConventions.h"

#if defined(HAVE_LIBCGNS)

// FIXME TT: cgnslib.h is not suited to namespaces as it is full of defines
// (hence the dirty redefine of csize_t). Namespace CGNS should be removed.
namespace CGNS {
#include <cgnslib.h>
#ifdef cgsize_t
#undef cgsize_t
  typedef int cgsize_t;
#endif
} // namespace CGNS


namespace {


// Type for periodic node correpondence (global indices)
typedef std::map<MVertex *, MVertex *> VertVertMap;

// Type for global node index -> (partition, local node index) correspondence
struct LocalData { unsigned int partition = 0; CGNS::cgsize_t index = 0; };
// typedef std::vector<LocalData> Global2LocalData;
typedef std::map<MVertex *, std::vector<LocalData> > Vertex2LocalData;


// Types for periodic and interface connectivities
typedef std::pair<unsigned int, unsigned int> PartitionInterface;
typedef std::pair<GEntity *, GEntity *> EntityInterface;
typedef std::pair<PartitionInterface, EntityInterface> PeriodicInterface;
typedef std::pair<std::vector<CGNS::cgsize_t>, std::vector<CGNS::cgsize_t> >
  NodeCorrespondence;
typedef std::map<PeriodicInterface, NodeCorrespondence> PeriodicConnection;
typedef std::map<PartitionInterface, NodeCorrespondence> PartitionConnection;


int cgnsError(const int cgIndexFile = -1)
{
  Msg::Error("Error detected by CGNS library");
  Msg::Error(CGNS::cg_get_error());
  if(cgIndexFile != -1) {
    if(CGNS::cg_close(cgIndexFile)) {
      Msg::Error("Unable to close CGNS file");
    }
  }
  return 0;
}


void printProgress(const char *cstr, size_t i, size_t num)
{
  if(num > 100) {
    if(i % 100 == 1) {
      Msg::Info("%s %d/%d", cstr, i, num);
    }
  }
  else {
    Msg::Info("%s %d/%d", cstr, i, num);
  }
}


// Retrieve all nodes (possibly including high-order ones) in the elements
// contained in the given entities
template <bool INCLUDE_HO_NODES>
void getNodesInEntities(const std::vector<GEntity *> &entities,
                        bool allElements, std::set<MVertex *> &nodeSet)
{
  for(std::size_t i = 0; i < entities.size(); i++) {
    GEntity *ge = entities[i];
    std::vector<int> eleTypes;
    ge->getElementTypes(eleTypes);
    for(std::size_t eleType = 0; eleType < eleTypes.size(); eleType++) {
      int numEle = ge->getNumMeshElementsByType(eleTypes[eleType]);
      if(numEle && (allElements || ge->physicals.size())) {
        for(int j = 0; j < numEle; j++) {
          MElement *me = ge->getMeshElementByType(eleTypes[eleType], j);
          int numEltVert = INCLUDE_HO_NODES ? me->getNumVertices() :
                                              me->getNumPrimaryVertices();
          for(int k = 0; k < numEltVert; k++) {
            nodeSet.insert(me->getVertex(k));
          }
        }
      }
    }
  }
}


// Initialize MVertex -> local data correspondence (only for primary vertices)
void initInterfVertex2LocalData(const std::vector<GEntity *> &entitiesPer,
                                const std::vector<GEntity *> &entitiesInterf,
                                Vertex2LocalData &interfVert2Local)
{
  // Periodic entities
  for(std::size_t i = 0; i < entitiesPer.size(); i++) {
    VertVertMap &vv = entitiesPer[i]->correspondingVertices;
    for(VertVertMap::iterator itV = vv.begin(); itV != vv.end(); itV++) {
      interfVert2Local[itV->first] = std::vector<LocalData>();
      interfVert2Local[itV->second] = std::vector<LocalData>();
    }
  }

  // Partition interface boundaries
  typedef std::set<MVertex *>::iterator NodeSetIter;
  std::set<MVertex *> nodeSet;
  getNodesInEntities<false>(entitiesInterf, true, nodeSet);
  for(NodeSetIter itN = nodeSet.begin(); itN != nodeSet.end(); ++itN) {
    interfVert2Local[*itN] = std::vector<LocalData>();
  }
}


// create periodic connections for all entities in all partitions
int writePeriodic(const std::vector<GEntity *> &entitiesPer, int cgIndexFile,
                   int cgIndexBase, const std::vector<std::string> &zoneName,
                   Vertex2LocalData &interfVert2Local)
{
  // Construct interfaces [slave (entity, part.) <-> master (entity, part.)]
  // with corresponding nodes
  Msg::Info("Constructing connectivities for %i periodic entities",
            entitiesPer.size());
  PeriodicConnection connect;
  typedef std::map<MVertex *, MVertex *> VertVertMap;
  for(std::size_t iEnt = 0; iEnt < entitiesPer.size(); iEnt++) {
    GEntity *slaveEnt = entitiesPer[iEnt];
    GEntity *masterEnt = slaveEnt->getMeshMaster();
    VertVertMap &vv = slaveEnt->correspondingVertices;
    for(VertVertMap::iterator itV = vv.begin(); itV != vv.end(); itV++) {
      const std::vector<LocalData> &allSlaveData = interfVert2Local[itV->first];
      const std::vector<LocalData> &allMasterData = interfVert2Local[itV->second];
      for (std::size_t iS = 0; iS < allSlaveData.size(); iS++) {
        const LocalData &slaveData = allSlaveData[iS];
        for (std::size_t iM = 0; iM < allMasterData.size(); iM++) {
          const LocalData &masterData = allMasterData[iM];
          PartitionInterface partInt = std::make_pair(slaveData.partition,
                                                      masterData.partition);
          EntityInterface entInt = std::make_pair(slaveEnt, masterEnt);
          PeriodicInterface perInt = std::make_pair(partInt, entInt);
          NodeCorrespondence &nc = connect[perInt];
          nc.first.push_back(slaveData.index);
          nc.second.push_back(masterData.index);
          PartitionInterface partInt2 = std::make_pair(masterData.partition,
                                                       slaveData.partition);
          EntityInterface entInt2 = std::make_pair(masterEnt, slaveEnt);
          PeriodicInterface perInt2 = std::make_pair(partInt2, entInt2);
          NodeCorrespondence &nc2 = connect[perInt2];
          nc2.first.push_back(masterData.index);
          nc2.second.push_back(slaveData.index);
        }
      }
    }
  }

  std::vector<float> rotCenter(3, 0.), rotAngle(3, 0.), trans(3, 0.);

  // write periodic interfaces
  typedef PeriodicConnection::iterator PerConnectIter;
  for(PerConnectIter it = connect.begin(); it != connect.end(); ++it) {
    printProgress("Writing periodic interface",
                  std::distance(connect.begin(), it)+1, connect.size());
    const PeriodicInterface &perInt = it->first;
    const PartitionInterface &partInt = perInt.first;
    const EntityInterface &entInt = perInt.second;
    const std::size_t &part1 = partInt.first;
    const int entTag1 = entInt.first->tag();
    const std::size_t &part2 = partInt.second;
    const int entTag2 = entInt.second->tag();
    const NodeCorrespondence &nc = it->second;
    const std::vector<CGNS::cgsize_t> &nodes1 = nc.first;
    const std::vector<CGNS::cgsize_t> &nodes2 = nc.second;
    int slaveZone = (part1 == 0) ? 1 : part1;
    const std::string &masterZoneName = zoneName[part2]; 
    std::ostringstream ossInt;
    ossInt << "Per_Part" << part1 << "_Ent" << entTag1
           << "_Part" << part2 << "_Ent" << entTag2;
    const std::string interfaceName = cgnsString(ossInt.str()); 
    int connIdx;
    CGNS::cg_conn_write(cgIndexFile, cgIndexBase, slaveZone,
                        interfaceName.c_str(), CGNS::Vertex, CGNS::Abutting1to1,
                        CGNS::PointList, nodes1.size(), nodes1.data(),
                        masterZoneName.c_str(), CGNS::Unstructured,
                        CGNS::PointListDonor, CGNS::DataTypeNull,
                        nodes2.size(), nodes2.data(), &connIdx);
    CGNS::cg_conn_periodic_write(cgIndexFile, cgIndexBase, slaveZone, connIdx,
                                 rotCenter.data(), rotAngle.data(),
                                 trans.data());
  }

  return 0;
}


// create partition interfaces
int writeInterfaces(const std::vector<GEntity *> &entitiesInterf,
                    int cgIndexFile, int cgIndexBase,
                    const std::vector<std::string> &zoneName,
                    Vertex2LocalData &interfVert2Local)
{
  // get nodes in partition interface entities
  typedef std::set<MVertex *>::iterator NodeSetIter;
  std::set<MVertex *> nodeSet;
  getNodesInEntities<false>(entitiesInterf, true, nodeSet);

  // construct (two-way) partition connectivities with corresponding nodes
  Msg::Info("Constructing connectivities for %i partition interface entities",
            entitiesInterf.size());
  PartitionConnection connect;
  for(NodeSetIter itN = nodeSet.begin(); itN != nodeSet.end(); ++itN) {
    const std::vector<LocalData> &allLocalData = interfVert2Local[*itN];
    for (std::size_t iLD1 = 0; iLD1 < allLocalData.size(); iLD1++) {
      const LocalData &localData1 = allLocalData[iLD1];
      for (std::size_t iLD2 = 0; iLD2 < allLocalData.size(); iLD2++) {
        if (iLD2 == iLD1) continue;
        const LocalData &localData2 = allLocalData[iLD2];
        std::pair<unsigned int, unsigned int> partInt(localData1.partition,
                                                      localData2.partition);
        NodeCorrespondence &nc = connect[partInt];
        nc.first.push_back(localData1.index);
        nc.second.push_back(localData2.index);
      }
    }
  }

  // write partition interfaces
  typedef PartitionConnection::iterator PartConnectIter;
  std::size_t iPartConnect = 0;
  for(PartConnectIter it = connect.begin(); it != connect.end(); ++it) {
    iPartConnect++;
    printProgress("Writing partition interface", iPartConnect, connect.size());
    const std::pair<unsigned int, unsigned int> &partInt = it->first;
    const std::size_t &part1 = partInt.first;
    const std::size_t &part2 = partInt.second;
    const NodeCorrespondence &nc = it->second;
    const std::string &masterZoneName = zoneName[part2]; 
    std::ostringstream ossInt;
    ossInt << "Part" << part1 << "_Part" << part2;
    const std::string interfaceName = cgnsString(ossInt.str()); 
    int dum;
    CGNS::cg_conn_write(cgIndexFile, cgIndexBase, part1,
                        interfaceName.c_str(), CGNS::Vertex, CGNS::Abutting1to1,
                        CGNS::PointList, nc.first.size(), nc.first.data(),
                        masterZoneName.c_str(), CGNS::Unstructured,
                        CGNS::PointListDonor, CGNS::DataTypeNull,
                        nc.second.size(), nc.second.data(), &dum);
  }

  return 0;
}


// create a single zone for a whole partition; nodes and elements are
// referenced with per-zone index (starting at 1) inside a zone
int writeZone(GModel *model, bool saveAll, double scalingFactor,
              int meshDim, size_t numNodesTotal, size_t partition,
              const std::vector<GEntity *> &entities, int cgIndexFile,
              int cgIndexBase, std::vector<std::string> &zoneName,
              Vertex2LocalData &interfVert2Local)
{
  typedef std::set<MVertex *>::iterator NodeSetIter;

  // build set of nodes first, use elements because nodes not all in
  // GEntity::mesh_vertices if entities do not include partition interfaces
  std::set<MVertex *> nodeSet;
  getNodesInEntities<true>(entities, saveAll, nodeSet);

  // build global -> partition-local node index correspondence and
  // store local data correspondence for periodic/interface nodes
  std::vector<LocalData> global2Local(numNodesTotal+1);
  CGNS::cgsize_t numNodes = 0;
  for(NodeSetIter itN = nodeSet.begin(); itN != nodeSet.end(); ++itN) {
    const long gInd = (*itN)->getIndex();
    if (gInd < 0) continue;
    numNodes++;
    LocalData &ld = global2Local[gInd];
    ld.partition = partition;
    ld.index = numNodes;
    Vertex2LocalData::iterator itPN = interfVert2Local.find(*itN);
    if (itPN != interfVert2Local.end()) itPN->second.push_back(ld);
  }

  // create lists of coordinates
  std::vector<double> xcoord(numNodes), ycoord(numNodes), zcoord(numNodes);
  for(NodeSetIter itN = nodeSet.begin(); itN != nodeSet.end(); ++itN) {
    const long gInd = (*itN)->getIndex();
    if (gInd < 0) continue;
    const int ln = global2Local[gInd].index;
    xcoord[ln - 1] = (*itN)->x() * scalingFactor;
    ycoord[ln - 1] = (*itN)->y() * scalingFactor;
    zcoord[ln - 1] = (*itN)->z() * scalingFactor;
  }

  // number of elements for highest spatial dimension
  CGNS::cgsize_t numElementsMaxDim = 0;
  for(std::size_t i = 0; i < entities.size(); i++) {
    GEntity *ge = entities[i];
    if(ge->dim() == meshDim && (saveAll || ge->physicals.size())) {
      numElementsMaxDim += ge->getNumMeshElements();
    }
  }

  // write zone CGNS node
  int cgIndexZone = 0;
  CGNS::cgsize_t cgZoneSize[3] = {numNodes, numElementsMaxDim, 0};
  std::ostringstream ossPart;
  ossPart << "_Part" << partition;
  std::string partSuffix = ossPart.str();
  std::string modelName = cgnsString(model->getName(), 32-partSuffix.size());
  zoneName[partition] = modelName + partSuffix;
  if(CGNS::cg_zone_write(cgIndexFile, cgIndexBase, zoneName[partition].c_str(),
                         cgZoneSize, CGNS::Unstructured, &cgIndexZone))
    return cgnsError();

  // create a CGNS grid with x, y and z coordinates of all the nodes (that are
  // referenced by elements)
  int cgIndexGrid = 0;
  if(CGNS::cg_grid_write(cgIndexFile, cgIndexBase, cgIndexZone,
                         "GridCoordinates", &cgIndexGrid))
    return cgnsError();

  // write list of coordinates
  int cgIndexCoord = 0;
  if(CGNS::cg_coord_write(cgIndexFile, cgIndexBase, cgIndexZone,
                          CGNS::RealDouble, "CoordinateX", &xcoord[0],
                          &cgIndexCoord))
    return cgnsError();
  if(CGNS::cg_coord_write(cgIndexFile, cgIndexBase, cgIndexZone,
                          CGNS::RealDouble, "CoordinateY", &ycoord[0],
                          &cgIndexCoord))
    return cgnsError();
  if(CGNS::cg_coord_write(cgIndexFile, cgIndexBase, cgIndexZone,
                          CGNS::RealDouble, "CoordinateZ", &zcoord[0],
                          &cgIndexCoord))
    return cgnsError();

  // write an element section for each entity, per element type (TODO: check if
  // using the actual element tag in case the numbering is dense and
  // saveAll==true would make sense/would be useful; maybe in the context of
  // partitioned meshes?)
  CGNS::cgsize_t eleStart = 0, eleEnd = 0;
  for(std::size_t i = 0; i < entities.size(); i++) {
    GEntity *ge = entities[i];

    // FIXME: use MIXED section? -> probably less efficient
    // 2) store physical information in a "family"?
    // get or create the name for the entity 
    std::string entityName = model->getElementaryName(ge->dim(), ge->tag());
    if(entityName.empty()) {
      std::ostringstream s;
      std::string entStr = (ge->dim() == 3) ? "Vol" :
                           (ge->dim() == 2) ? "Surf" :
                           (ge->dim() == 1) ? "Curve" :
                           (ge->dim() == 0) ? "Pnt" : ""; 
      s << "Gmsh_" << entStr << "_" << ge->tag();
      entityName = s.str();
    }
    entityName = cgnsString(entityName);

    // retrieve element types for this geometric entity
    std::vector<int> eleTypes;
    ge->getElementTypes(eleTypes);

    // build list of connectivities
    for(std::size_t eleType = 0; eleType < eleTypes.size(); eleType++) {
      int numEle = ge->getNumMeshElementsByType(eleTypes[eleType]);
      if(numEle && (saveAll || ge->physicals.size())) {
        eleStart = (eleEnd + 1);
        eleEnd += numEle;
        MElement *me = ge->getMeshElementByType(eleTypes[eleType], 0);
        int mshType = me->getTypeForMSH();
        CGNS::ElementType_t cgType = msh2CgnsEltType(mshType);
        if(cgType == CGNS::ElementTypeNull) {
          Msg::Error("Unhandled element type in CGNS ouput (%d)", mshType);
          break;
        }
        std::vector<int> mshNodeInd = cgns2MshNodeIndex(mshType);
        
        int numNodesPerEle = me->getNumVertices();
        std::vector<CGNS::cgsize_t> elemNodes(numEle * numNodesPerEle);
        std::size_t n = 0;
        for(int j = 0; j < numEle; j++) {
          me = ge->getMeshElementByType(eleTypes[eleType], j);
          for(int k = 0; k < numNodesPerEle; k++) {
            const int gInd = me->getVertex(mshNodeInd[k])->getIndex();
            elemNodes[n] = global2Local[gInd].index;
            n++;
          }
        }

        std::ostringstream ossSection(entityName);
        ossSection << "_" << eleTypes[eleType];
        int cgIndexSection = 0;
        if(CGNS::cg_section_write(cgIndexFile, cgIndexBase, cgIndexZone,
                                  ossSection.str().c_str(), cgType, eleStart,
                                  eleEnd, 0, &elemNodes[0], &cgIndexSection))
          return cgnsError();
      }
    }
  }

  return 0;
}


// Get all partition interface entities
void getPeriodicEntities(const std::vector<GEntity *> &entities,
                         std::vector<GEntity *> &entitiesPer)
{
  for(std::size_t i = 0; i < entities.size(); i++) {
    GEntity *slaveEnt = entities[i];
    GEntity *masterEnt = slaveEnt->getMeshMaster();
    if (slaveEnt != masterEnt) entitiesPer.push_back(slaveEnt);
  }
}


// Get all partition interface entities
void getPartitionInterfaceEntities(const std::vector<GEntity *> &entities,
                                   std::vector<GEntity *> &entitiesInt)
{
  for(std::size_t j = 0; j < entities.size(); j++) {
    GEntity *ge = entities[j];
    switch(ge->geomType()) {
    case GEntity::PartitionVolume: {
      partitionRegion *pr = static_cast<partitionRegion *>(ge);
      if (pr->getParentEntity()->dim() != pr->dim()) entitiesInt.push_back(ge);
    } break;
    case GEntity::PartitionSurface: {
      partitionFace *pf = static_cast<partitionFace *>(ge);
      if (pf->getParentEntity()->dim() != pf->dim()) entitiesInt.push_back(ge);
    } break;
    case GEntity::PartitionCurve: {
      partitionEdge *pe = static_cast<partitionEdge *>(ge);
      if (pe->getParentEntity()->dim() != pe->dim()) entitiesInt.push_back(ge);
    } break;
    case GEntity::PartitionPoint: {
      partitionVertex *pv = static_cast<partitionVertex *>(ge);
      if (pv->getParentEntity()->dim() != pv->dim()) entitiesInt.push_back(ge);
    } break;
    default:
      break;
    }         // switch
  }           // loop on entities
}


// Get entities in each partition, except partition interfaces
void getEntitiesInPartitions(const std::vector<GEntity *> &entities,
                             std::vector<std::vector<GEntity *> > &entitiesPart)
{
  for(std::size_t j = 0; j < entities.size(); j++) {
    GEntity *ge = entities[j];
    const std::vector<unsigned int> *parts = 0;
    switch(ge->geomType()) {
    case GEntity::PartitionVolume: {
      partitionRegion *pr = static_cast<partitionRegion *>(ge);
      if (pr->getParentEntity()->dim() != pr->dim()) continue;  // Skip partition interfaces
      parts = &(pr->getPartitions());
    } break;
    case GEntity::PartitionSurface: {
      partitionFace *pf = static_cast<partitionFace *>(ge);
      if (pf->getParentEntity()->dim() != pf->dim()) continue;  // Skip partition interfaces
      parts = &(pf->getPartitions());
    } break;
    case GEntity::PartitionCurve: {
      partitionEdge *pe = static_cast<partitionEdge *>(ge);
      if (pe->getParentEntity()->dim() != pe->dim()) continue;  // Skip partition interfaces
      parts = &(pe->getPartitions());
    } break;
    case GEntity::PartitionPoint: {
      partitionVertex *pv = static_cast<partitionVertex *>(ge);
      if (pv->getParentEntity()->dim() != pv->dim()) continue;  // Skip partition interfaces
      parts = &(pv->getPartitions());
    } break;
    default:
      break;
    }         // switch
    if (parts != 0) {
      for (std::size_t iPart = 0; iPart < parts->size(); iPart++) {
        entitiesPart[(*parts)[iPart]].push_back(ge);
      }
    }
  }           // loop on entities
}


} // anonymous namespace


int GModel::readCGNS(const std::string &name) { return 0; }

int GModel::writeCGNS(const std::string &name, bool saveAll,
                      double scalingFactor)
{
  int cgIndexFile = 0;
  if(CGNS::cg_open(name.c_str(), CG_MODE_WRITE, &cgIndexFile))
    return cgnsError();

  // write the base node
  int meshDim = getMeshDim(), dim = getDim(), cgIndexBase = 0;
  size_t posStartName = name.find_last_of("/\\");
  if (posStartName == std::string::npos) posStartName = 0;
  else posStartName++;
  std::string baseName = cgnsString(name.substr(posStartName));
  if(CGNS::cg_base_write(cgIndexFile, baseName.c_str(), meshDim, dim,
                         &cgIndexBase))
    return cgnsError();

  // write information about who generated the mesh
  if(CGNS::cg_goto(cgIndexFile, cgIndexBase, "end")) return cgnsError();
  if(CGNS::cg_descriptor_write("About", "Created by Gmsh")) return cgnsError();

  // Zone names
  const size_t numPart = getNumPartitions();
  std::vector<std::string> zoneName(numPart+1);

  // index mesh nodes, as CGNS does not store node tags
  if(noPhysicalGroups()) saveAll = true;
  const size_t numNodes = indexMeshVertices(saveAll);

  // get entities from model
  std::vector<GEntity *> entities, entitiesPer, entitiesInterf;
  getEntities(entities);
  getPeriodicEntities(entities, entitiesPer);
  if (numPart > 0) {
    getPartitionInterfaceEntities(entities, entitiesInterf);
    if (entitiesInterf.size() == 0) {
      Msg::Error("Partitioned entities not detected");
    }
  }

  // initialize vertex -> (partition, local node index) for periodic and
  // interface entities
  Msg::Info("Looking for periodic/interface vertices");
  Vertex2LocalData interfVert2Local;
  initInterfVertex2LocalData(entitiesPer, entitiesInterf, interfVert2Local);
  Msg::Info("%i periodic/interface vertices found", interfVert2Local.size());

  // write partitions and periodic/partition connectivities
  if (numPart == 0) {                                   // mesh not partitioned
    int err = writeZone(this, saveAll, scalingFactor, meshDim, numNodes, 0,
                        entities, cgIndexFile, cgIndexBase, zoneName,
                        interfVert2Local);
    if(err) return err;
    if (entitiesPer.size() > 0) {
      err = writePeriodic(entitiesPer, cgIndexFile, cgIndexBase, zoneName,
                          interfVert2Local);
      if(err) return err;
    }
  }
  else {                                                // partitioned mesh
    std::vector<std::vector<GEntity *> > entitiesPart;
    entitiesPart.resize(numPart+1);
    getEntitiesInPartitions(entities, entitiesPart);
    for(std::size_t iPart = 1; iPart <= numPart; iPart++) {
      printProgress("Writing partition", iPart, numPart);
      int err = writeZone(this, saveAll, scalingFactor, meshDim, numNodes,
                          iPart, entitiesPart[iPart], cgIndexFile, cgIndexBase,
                          zoneName, interfVert2Local);
      if(err) return err;
    }             // loop on partitions
    if (entitiesPer.size() > 0) {
      int err = writePeriodic(entitiesPer, cgIndexFile, cgIndexBase, zoneName,
                              interfVert2Local);
      if(err) return err;
    }
    if (entitiesInterf.size() > 0) {
      int err = writeInterfaces(entitiesInterf, cgIndexFile, cgIndexBase,
                                zoneName, interfVert2Local);
      if(err) return err;
    }
  }   // numPart == 0

  if(CGNS::cg_close(cgIndexFile)) return cgnsError();

  return 0;
}

#else

int GModel::readCGNS(const std::string &name)
{
  Msg::Error("This version of Gmsh was compiled without CGNS support");
  return 0;
}

int GModel::writeCGNS(const std::string &name, bool saveAll,
                      double scalingFactor)
{
  Msg::Error("This version of Gmsh was compiled without CGNS support");
  return 0;
}

#endif
