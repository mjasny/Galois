/** Async Betweenness centrality -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * Asynchrounous betweeness-centrality. 
 *
 * @author Dimitrios Prountzos <dprountz@cs.utexas.edu> (Main code writer)
 * @author Loc Hoang <l_hoang@utexas.edu>
 */

#define NDEBUG // Used in Debug build to prevent things from printing

#include "Lonestar/BoilerPlate.h"
#include "galois/ConditionalReduction.h"

#include <boost/tuple/tuple.hpp>

#include <iostream>
#include <fstream>
#include <sstream>

#include "BCGraph.h"

////////////////////////////////////////////////////////////////////////////////
// Command line parameters
////////////////////////////////////////////////////////////////////////////////

namespace cll = llvm::cl;

static cll::opt<std::string> filename(cll::Positional,
                                      cll::desc("<input graph in Galois bin "
                                                "format>"),
                                      cll::Required);

static cll::opt<unsigned int> startNode("startNode", 
                                        cll::desc("Node to start search from"),
                                        cll::init(0));

static cll::opt<bool> generateCert("generateCertificate",
                                   cll::desc("Prints certificate at end of "
                                             "execution"),
                                   cll::init(false));
// TODO better description
//static cll::opt<bool> useNodeBased("useNodeBased",
//                                   cll::desc("Use node based execution"),
//                                   cll::init(true));

using NodeType = typename BCGraph::NodeType;
struct BetweenessCentralityAsync {
  BCGraph& graph;
  galois::LargeArray<NodeType>& gnodes;
  NodeType* currSrcNode;

  BetweenessCentralityAsync(BCGraph& _graph) 
      : graph(_graph), gnodes(_graph.getNodes()) { }
  
  using Counter = 
    ConditionalAccumulator<galois::GAccumulator<unsigned long>, BC_COUNT_ACTIONS>;
  Counter spfuCount;
  Counter updateSigmaP1Count;
  Counter updateSigmaP2Count;
  Counter firstUpdateCount;
  Counter correctNodeP1Count;
  Counter correctNodeP2Count;
  Counter noActionCount;
  
  using MaxCounter = 
    ConditionalAccumulator<galois::GReduceMax<unsigned long>, BC_COUNT_ACTIONS>;
  MaxCounter largestNodeDist;
  
  using LeafCounter = 
    ConditionalAccumulator<galois::GAccumulator<unsigned long>, BC_COUNT_LEAVES>;
  LeafCounter leafCount;

  void CorrectNode(NodeType* B, BCEdge& ed, BCEdge* edgeData) {
    int idx = B->id;
    int* inIdx = graph.inIdx;
    int* ins = graph.ins; 
    int startInE = inIdx[idx];
    int endInE = inIdx[idx + 1];

    // loop through in edges
    for (int j = startInE; j < endInE; j++) {
      BCEdge & inE = edgeData[ins[j]];
      NodeType *inNbr = inE.src;
      if (inNbr == B) continue;

      // lock in right order
      NodeType* aL;
      NodeType* aW;
      if (B < inNbr) { aL = B; aW = inNbr;} 
      else { aL = inNbr; aW = B; }
      aL->lock(); aW->lock();

      const unsigned elev = inE.level; 
      // Correct Node
      if (inNbr->distance >= B->distance) { 
        correctNodeP1Count.update(1);
        B->unlock();

        galois::gDebug("Rule 4 (", inNbr->toString(), " ||| ", 
                       B->toString(), ")", ed.level);

        if (elev != infinity) {
          inE.level = infinity;
          if (elev == inNbr->distance) {
            correctNodeP2Count.update(1);
            inNbr->nsuccs--;
          }
        }
        inNbr->unlock();
      } else {
        aL->unlock(); aW->unlock();
      }
    }
  }

  template<typename CTXType>
  void spAndFU(NodeType* A, NodeType* B, BCEdge& ed, BCEdge* edgeData, 
               CTXType& ctx) {
    spfuCount.update(1);

    // make B a successor of A, A predecessor of B
    A->nsuccs++;
    const double ASigma = A->sigma;
    A->unlock();
    NodeType::predTY& Bpreds = B->preds;
    bool bpredsNotEmpty = !Bpreds.empty();
    Bpreds.clear();
    Bpreds.push_back(A);
    B->distance = A->distance + 1;

    largestNodeDist.update(B->distance);

    B->nsuccs = 0; // SP
    B->sigma = ASigma; // FU
    ed.val = ASigma;
    ed.level = A->distance;
    B->unlock();

    if (!B->isAlreadyIn()) ctx.push(B);

    if (bpredsNotEmpty) {
      CorrectNode(B, ed, edgeData);
    }
  }

  template<typename CTXType>
  void updateSigma(NodeType* A, NodeType* B, BCEdge& ed, CTXType& ctx) {
    updateSigmaP1Count.update(1);
    galois::gDebug("Rule 2 (", A->toString(), " ||| ", B->toString(), ") ",
                   ed.level);
    const double ASigma = A->sigma;
    const double eval = ed.val;
    const double diff = ASigma - eval;
    bool BSigmaChanged = diff >= 0.00001;
    A->unlock();
    if (BSigmaChanged) {
      updateSigmaP2Count.update(1);
      ed.val = ASigma;
      B->sigma += diff;
      int nbsuccs = B->nsuccs;
      B->unlock();
      if (nbsuccs > 0) {
        if (!B->isAlreadyIn()) ctx.push(B);
      }
    } else {
      B->unlock();
    }
  }

  template<typename CTXType>
  void FirstUpdate(NodeType* A, NodeType* B, BCEdge& ed, CTXType& ctx) {
    firstUpdateCount.update(1);
    A->nsuccs++;
    const double ASigma = A->sigma;
    A->unlock();
  
    //if (AnotPredOfB) {
    B->preds.push_back(A);
    //}
    const double BSigma = B->sigma;
    galois::gDebug("Rule 3 (", A->toString(), " ||| ", B->toString(), 
                   ") ", ed.level);
    B->sigma = BSigma + ASigma;
    ed.val = ASigma;
    ed.level = A->distance;
    int nbsuccs = B->nsuccs;
    B->unlock();
    //const bool BSigmaChanged = ASigma >= 0.00001;
    if (nbsuccs > 0 /*&& BSigmaChanged*/) {
      if (!B->isAlreadyIn()) ctx.push(B);
    }
  }
  
  template <typename WLForEach, typename WorkListType>
  void dagConstruction(WorkListType& wl) {
    galois::for_each(
      galois::iterate(wl), 
      [&] (NodeType* srcD, auto& ctx) {
        int idx = srcD->id;
        srcD->markOut();
  
        int* outIdx = graph.outIdx;
        int startE = outIdx[idx];
        int endE = outIdx[idx + 1];
        BCEdge* edgeData = graph.edgeData;
  
        // loop through all edges
        for (int i = startE; i < endE; i++) {
          BCEdge& ed = edgeData[i];
          NodeType* dstD = ed.dst;
          
          if (srcD == dstD) continue; // ignore self loops
  
          // lock in set order to prevent deadlock (lower id first)
          // TODO run even in serial version; find way to not need to run
          NodeType* loser;
          NodeType* winner;
  
          if (srcD < dstD) { 
            loser = srcD; winner = dstD;
          } else { 
            loser = dstD; winner = srcD;
          }
  
          loser->lock();
          winner->lock();
  
          const int elevel = ed.level;
          NodeType* A = srcD;
          const int ADist = srcD->distance;
          NodeType* B = dstD;
          const int BDist = dstD->distance;
  
          if (BDist - ADist > 1) {
            // Shortest Path + First Update (and Correct Node)
            this->spAndFU(A, B, ed, edgeData, ctx);
          } else if (elevel == ADist && BDist == ADist + 1) {
            // Update Sigma
            this->updateSigma(A, B, ed, ctx);
          } else if (BDist == ADist + 1 && elevel != ADist) {
            // First Update not combined with Shortest Path
            this->FirstUpdate(A, B, ed, ctx);
          } else { // No Action
            noActionCount.update(1);
            A->unlock(); B->unlock();
          }
        }
      },      
      galois::wl<WLForEach>()
    );
  }
  
  template <typename WorkListType>
  void dependencyBackProp(WorkListType& wl) {
    galois::for_each(
      galois::iterate(wl),
      [&] (NodeType* A, auto& ctx) {
        A->lock();
  
        if (A->nsuccs == 0) {
          const double Adelta = A->delta;
          A->bc += Adelta;
  
          A->unlock();
  
          NodeType::predTY& Apreds = A->preds;
          int sz = Apreds.size();
  
          // loop through A's predecessors
          for (int i = 0; i < sz; ++i) {
            NodeType* pd = Apreds[i];
            const double term = pd->sigma * (1.0 + Adelta) / A->sigma; 
            pd->lock();
            pd->delta += term;
            const int prevPdNsuccs = pd->nsuccs;
            pd->nsuccs--;
  
            if (prevPdNsuccs == 1) {
              pd->unlock();
              ctx.push(pd);
            } else {
              pd->unlock();
            }
          }
          A->reset();
          graph.resetOutEdges(A);
        } else {
          galois::gDebug("Skipped ", A->toString());
          A->unlock();
        }
      }
    );
  }
  
  galois::InsertBag<NodeType*>* fringewl;
  
  void findLeaves(unsigned nnodes) {
    galois::do_all(
      galois::iterate(0u, nnodes),
      [&] (auto i) {
        NodeType* n = &(gnodes[i]);
        if (n->nsuccs == 0 && n->distance < infinity) {
          leafCount.update(1);
              
          fringewl->push(n);
        }
      }
    );
  }
};

struct NodeIndexer : std::binary_function<NodeType*, int, int> {
  int operator() (const NodeType *val) const {
    return val->distance;
  }
};

static const char* name = "Betweenness Centrality";
static const char* desc = "Computes betwenness centrality in an unweighted "
                          "graph";

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, NULL);

  if (BC_CONCURRENT) {
    galois::gInfo("Running in concurrent mode with ", numThreads, " threads");
  } else {
    galois::gInfo("Running in serial mode");
  }

  BCGraph graph(filename.c_str());

  BetweenessCentralityAsync bcExecutor(graph);

  unsigned nnodes = graph.size();
  unsigned nedges = graph.getNedges();
  galois::gInfo("Num nodes is ", nnodes, ", num edges is ", nedges);

  bcExecutor.spfuCount.reset();
  bcExecutor.updateSigmaP1Count.reset();
  bcExecutor.updateSigmaP2Count.reset();
  bcExecutor.firstUpdateCount.reset();
  bcExecutor.correctNodeP1Count.reset();
  bcExecutor.correctNodeP2Count.reset();
  bcExecutor.noActionCount.reset();
  bcExecutor.largestNodeDist.reset();

  const int chunksize = 8;
  galois::gInfo("Using chunk size : ", chunksize);
  typedef galois::worklists::OrderedByIntegerMetric<NodeIndexer, 
                           galois::worklists::dChunkedLIFO<chunksize> > wl2ty;
  galois::InsertBag<NodeType*> wl2;

  galois::InsertBag<NodeType*> wl4;
  bcExecutor.fringewl = &wl4;

  galois::reportPageAlloc("MemAllocPre");
  galois::preAlloc(galois::getActiveThreads() * nnodes / 1650);
  galois::reportPageAlloc("MemAllocMid");

  unsigned stepCnt = 0; // number of sources done

  galois::StatTimer executionTimer;
  galois::StatTimer forwardPassTimer("ForwardPass", "BC");
  galois::StatTimer leafFinderTimer("LeafFind", "BC");
  galois::StatTimer backwardPassTimer("BackwardPass", "BC");
  galois::StatTimer cleanupTimer("CleanupTimer", "BC");

  // reset everything in preparation for run
  graph.cleanupData();
    
  executionTimer.start();
  for (unsigned i = startNode; i < nnodes; ++i) {
    NodeType* active = &(bcExecutor.gnodes[i]);
    bcExecutor.currSrcNode = active;
    // ignore nodes with no neighbors
    int nnbrs = graph.outNeighborsSize(active);
    if (nnbrs == 0) {
      continue;
    }

    stepCnt++;
    //if (stepCnt >= 2) break;  // ie only do 1 source

    std::vector<NodeType*>  wl;
    wl2.push_back(active);
    active->initAsSource();
    galois::gDebug("Source is ", active->toString());
    forwardPassTimer.start();

    bcExecutor.dagConstruction<wl2ty>(wl2);
    wl2.clear();

    forwardPassTimer.stop();
    if (DOCHECKS) graph.checkGraph(active);

    leafFinderTimer.start();
    bcExecutor.leafCount.reset();
    bcExecutor.findLeaves(nnodes);
    leafFinderTimer.stop();

    if (bcExecutor.leafCount.isActive()) {
      galois::gPrint(bcExecutor.leafCount.reduce(), " leaf nodes in DAG\n");
    }

    backwardPassTimer.start();
    double backupSrcBC = bcExecutor.currSrcNode->bc;
    bcExecutor.dependencyBackProp(wl4);

    bcExecutor.currSrcNode->bc = backupSrcBC; // current source BC should not get updated
    backwardPassTimer.stop();
    wl4.clear();
    if (DOCHECKS) graph.checkSteadyState2();

    cleanupTimer.start();
    graph.cleanupData();
    cleanupTimer.stop();
  }
  executionTimer.stop();

  galois::reportPageAlloc("MemAllocPost");

  // one counter active -> all of them are active (since all controlled by same
  // ifdef)
  if (bcExecutor.spfuCount.isActive()) {
    galois::gPrint("SP&FU ", bcExecutor.spfuCount.reduce(), 
                   "\nUpdateSigmaBefore ", bcExecutor.updateSigmaP1Count.reduce(), 
                   "\nRealUS ", bcExecutor.updateSigmaP2Count.reduce(), 
                   "\nFirst Update ", bcExecutor.firstUpdateCount.reduce(), 
                   "\nCorrectNodeBefore ", bcExecutor.correctNodeP1Count.reduce(), 
                   "\nReal CN ", bcExecutor.correctNodeP2Count.reduce(), 
                   "\nNoAction ", bcExecutor.noActionCount.reduce(), "\n");
    galois::gPrint("Largest node distance is ", bcExecutor.largestNodeDist.reduce(), "\n");
  }

  // prints out first 10 node BC values
  if (!skipVerify) {
    //graph.verify(); // TODO see what this does
    int count = 0;
    for (unsigned i = 0; i < nnodes && count < 10; ++i, ++count) {
      galois::gPrint(count, ": ", std::setiosflags(std::ios::fixed), 
                     std::setprecision(6), bcExecutor.gnodes[i].bc, "\n");
    }
  }

  if (generateCert) {
    graph.printAllBCs(numThreads, "certificate_");
  }

  return 0;
}
