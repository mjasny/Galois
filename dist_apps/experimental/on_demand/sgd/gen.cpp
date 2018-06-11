/**
 * This file belongs to the Galois project, a C++ library for exploiting parallelism.
 * The code is being released under the terms of XYZ License (a copy is located in
 * LICENSE.txt at the top-level directory).
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
 */

#include <iostream>
#include <limits>
#include <cmath>
#include "galois/DistGalois.h"
#include "galois/gstl.h"
#include "DistBenchStart.h"

#include "galois/DReducible.h"
#include "galois/AtomicWrapper.h"
#include "galois/ArrayWrapper.h"
#include "galois/runtime/Tracer.h"
#if 0
#include "galois/CompilerHelpers.h"
#endif
#include "galois/graphs/DistributedGraphLoader.h"

#ifdef __GALOIS_HET_CUDA__
#include "galois/cuda/cuda_device.h"
#include "gen_cuda.h"
struct CUDA_Context *cuda_ctx;
#endif

constexpr static const char* const regionname = "SGD";

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;

static cll::opt<unsigned int> maxIterations("maxIterations", 
    cll::desc("Maximum iterations: Default 10000"), 
    cll::init(10000));
static cll::opt<bool> bipartite("bipartite", 
    cll::desc("Is graph bipartite? if yes, it expects first N nodes to have "
              "edges."), 
    cll::init(false));
static cll::opt<double> LEARNING_RATE("LEARNING_RATE", 
    cll::desc("Learning rate (GAMMA): Default 0.00001"), 
    cll::init(0.00001));
static cll::opt<double> LAMBDA("LAMBDA",
    cll::desc("LAMBDA: Default 0.0001"),
    cll::init(0.0001));
static cll::opt<double> DECAY_RATE("DECAY_RATE",
    cll::desc("Decay rate to be used in step size function (DECAY_RATE): Default 0.9"),
    cll::init(0.9));
/******************************************************************************/
/* Graph structure declarations + helper functions + other initialization */
/******************************************************************************/

#define LATENT_VECTOR_SIZE 20

static const double MINVAL = -1e+100;
static const double MAXVAL = 1e+100;

const unsigned int infinity = std::numeric_limits<unsigned int>::max() / 4;

struct NodeData {

  std::vector<galois::CopyableAtomic<double>> residual_latent_vector;
  std::vector<double> residual_latent_vector2;
  std::vector<double> latent_vector;
};


typedef galois::graphs::DistGraph<NodeData, double> Graph;
typedef typename Graph::GraphNode GNode;

#include "gen_sync.hh"

//TODO: Set seed
static double genRand() {
  // generate a random double in (-1,1)
  return 2.0 * ((double)std::rand() / (double)RAND_MAX) - 1.0;
}

// Purdue learning function
double getstep_size(unsigned int round) {
  return LEARNING_RATE * 1.5 / (1.0 + DECAY_RATE * pow(round + 1, 1.5));
}

/**
 * Prediction of edge weight based on 2 latent vectors
 */
double calcPrediction (const NodeData& movie_data, const NodeData& user_data) {
  double pred = galois::innerProduct(movie_data.latent_vector, 
                                     user_data.latent_vector, 
                                     0.0); 
  double p = pred;

  pred = std::min(MAXVAL, pred);
  pred = std::max(MINVAL, pred);

  #ifndef NDEBUG
  if (p != pred)
    std::cerr << "clamped " << p << " to " << pred << "\n";
  #endif

  return pred;
}

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

//Global variable: For bistset optimization to track changes to the field.
galois::DynamicBitSet bitset_residual_latent_vector;
//Global variable: For on-demand communication optimization

struct InitializeGraph {
  Graph *graph;

  InitializeGraph(Graph* _graph) : graph(_graph){}

  void static go(Graph& _graph) {
    auto& allNodes = _graph.allNodesRange();

    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		std::string impl_str("InitializeGraph_" + (_graph.get_run_identifier()));
    		galois::StatTimer StatTimer_cuda(impl_str.c_str());
    		StatTimer_cuda.start();
    		InitializeGraph_all_cuda(cuda_ctx);
    		StatTimer_cuda.stop();
    	} else if (personality == CPU)
    #endif
    galois::do_all(galois::iterate(allNodes.begin(), allNodes.end()), 
                   InitializeGraph {&_graph}, galois::loopname("Init"));

  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);

    //resize vectors
    sdata.latent_vector.resize(LATENT_VECTOR_SIZE);
    sdata.residual_latent_vector.resize(LATENT_VECTOR_SIZE);
    sdata.residual_latent_vector2.resize(LATENT_VECTOR_SIZE);

    for (int i = 0; i < LATENT_VECTOR_SIZE; i++) {
      sdata.latent_vector[i] = genRand(); // randomly create latent vector 
      sdata.residual_latent_vector[i] = 0 ; // randomly create latent vector 
      sdata.residual_latent_vector2[i] = genRand(); //0 ; // randomly create latent vector 

      #ifndef NDEBUG
      if(!std::isnormal(sdata.latent_vector[i]))
        galois::gDebug("GEN for ", i, " ",  sdata.latent_vector[i]);
      #endif
    }
  }
};

struct SGD_mergeResidual {
  Graph* graph;

  SGD_mergeResidual(Graph* _graph) :
      graph(_graph){}

  void static go(Graph& _graph) {

    auto& allNodes = _graph.allNodesRange();
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		std::string impl_str("SGD_mergeResidual_" + (_graph.get_run_identifier()));
    		galois::StatTimer StatTimer_cuda(impl_str.c_str());
    		StatTimer_cuda.start();
    		SGD_mergeResidual_all_cuda(cuda_ctx);
    		StatTimer_cuda.stop();
    	} else if (personality == CPU)
    #endif
    galois::do_all(
          galois::iterate(allNodes.begin(), allNodes.end()),
          SGD_mergeResidual { &_graph },
          galois::loopname(_graph.get_run_identifier("SGD_merge").c_str()),
          galois::steal(),
          galois::no_stats());
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    //auto& latent_vector = sdata.latent_vector;
    //auto& residual_latent_vector = sdata.residual_latent_vector;

    for (int i = 0; i < LATENT_VECTOR_SIZE; ++i) {
      sdata.latent_vector[i] += sdata.residual_latent_vector[i];
      sdata.residual_latent_vector[i] = 0;
    }
  }
};

struct SGD {
  Graph* graph;
  double step_size;
  galois::DGAccumulator<double>& DGAccumulator_accum;

  SGD(Graph* _graph, double _step_size, galois::DGAccumulator<double>& _dga) : 
      graph(_graph), step_size(_step_size), DGAccumulator_accum(_dga) {}

  void static go(Graph& _graph, galois::DGAccumulator<double>& dga) {
    unsigned _num_iterations = 0;
    double rms_normalized = 0.0;
    auto& nodesWithEdges = _graph.allNodesWithEdgesRange();
    do {
      galois::gPrint("ITERATION : ",  _num_iterations, "\n");
      auto step_size = getstep_size(_num_iterations);
      dga.reset();
      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		std::string impl_str("SGD_" + (_graph.get_run_identifier()));
      		galois::StatTimer StatTimer_cuda(impl_str.c_str());
      		StatTimer_cuda.start();
      		SGD_all_cuda(cuda_ctx);
      		StatTimer_cuda.stop();
      	} else if (personality == CPU)
      #endif
      
    #if __OPT_VERSION__ == 5
          //TODO: Use bitset?
        //_graph.sync_on_demand<readAny , Reduce_pair_wise_add_array_residual_latent_vector,Broadcast_residual_latent_vector ,Bitset_residual_latent_vector>(Flags_residual_latent_vector, "SGD");
      	_graph.sync_on_demand<readAny , Reduce_pair_wise_add_array_residual_latent_vector,Broadcast_residual_latent_vector >(Flags_residual_latent_vector, "SGD");
    #endif
      
      galois::do_all(
          galois::iterate(nodesWithEdges),
          SGD( &_graph, step_size, dga),
          galois::loopname(_graph.get_run_identifier("SGD").c_str()),
          galois::steal(),
          galois::no_stats(), galois::write_set("sync_push", "this->graph", "struct NodeData &", "class std::vector<class galois::CopyableAtomic<double>, class std::allocator<class galois::CopyableAtomic<double> > >" , "residual_latent_vector", "class std::vector<class galois::CopyableAtomic<double>, class std::allocator<class galois::CopyableAtomic<double> > >" , "pair_wise_add_array",  "{node.residual_latent_vector = 0 ; }",  "readAny",  "writeAny"));
    #if __OPT_VERSION__ == 5
          Flags_residual_latent_vector.set_write_any();
    #endif


      SGD_mergeResidual::go(_graph);

      ++_num_iterations;

      // calculate root mean squared error
      rms_normalized = std::sqrt(dga.reduce() /
                                 _graph.globalSizeEdges());
      galois::gDebug("RMS Normalized : ", rms_normalized);
      galois::gPrint("RMS : ", rms_normalized, "\n");
    } while((_num_iterations < maxIterations) && (rms_normalized > 0.1));

  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    auto& movie_node = sdata.latent_vector;
    auto& residual_movie_node = sdata.residual_latent_vector;

    for (auto jj = graph->edge_begin(src), ej = graph->edge_end(src);
         jj != ej;
         ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& ddata = graph->getData(dst);

      auto& user_node = ddata.latent_vector;
      auto& residual_user_node = ddata.residual_latent_vector;
      //auto& sdata_up = sdata.updates;

      double edge_rating = graph->getEdgeData(dst);

      // doGradientUpdate
      double old_dp = galois::innerProduct(user_node, movie_node, double(0));

      double cur_error = edge_rating - old_dp;
      DGAccumulator_accum += (cur_error * cur_error);

      assert(cur_error < 10000 && cur_error > -10000);

      // update both vectors based on error derived from 2 previous vectors
      for (int i = 0; i < LATENT_VECTOR_SIZE; ++i) {

        double prevUser = user_node[i];
        double prevMovie = movie_node[i];

        galois::atomicAdd(residual_user_node[i],  double(step_size * (cur_error * prevMovie - LAMBDA * prevUser)));
        assert(std::isnormal(residual_user_node[i]));

        galois::atomicAdd(residual_movie_node[i],  double(step_size * (cur_error * prevUser - LAMBDA * prevMovie)));
        assert(std::isnormal(residual_movie_node[i]));
      }
    }
  }
};

/******************************************************************************/
/* Main */
/******************************************************************************/
constexpr static const char* const name = "SGD - Distributed Heterogeneous";
constexpr static const char* const desc = "SGD on Distributed Galois.";
constexpr static const char* const url = 0;

int main(int argc, char** argv) {
  galois::DistMemSys G;
  DistBenchStart(argc, argv, name, desc, url);

  const auto& net = galois::runtime::getSystemNetworkInterface();
  if (net.ID == 0) {
    galois::runtime::reportParam(regionname, "Max Iterations", 
        (unsigned long)maxIterations);
  }

  galois::StatTimer StatTimer_total("TimerTotal", regionname); 

  StatTimer_total.start();
#ifdef __GALOIS_HET_CUDA__
  Graph* hg = distGraphInitialization<NodeData, double>(&cuda_ctx);
#else
  Graph* hg = distGraphInitialization<NodeData, double>();
#endif

  galois::gPrint("[", net.ID, "] InitializeGraph::go called\n");

  galois::StatTimer StatTimer_init("TIMER_GRAPH_INIT", regionname); 
  StatTimer_init.start();
  InitializeGraph::go((*hg));
  StatTimer_init.stop();

  galois::runtime::getHostBarrier().wait();

  // accumulators for use in operators
  galois::DGAccumulator<double> DGAccumulator_accum;
  //galois::DGAccumulator<uint64_t> DGAccumulator_sum;
  //galois::DGAccumulator<uint32_t> DGAccumulator_max;
  //galois::GReduceMax<uint32_t> m;

  for (auto run = 0; run < numRuns; ++run) {
    galois::gPrint("[", net.ID, "] SGD::go run ", run, " called\n");
    std::string timer_str("Timer_" + std::to_string(run));
    galois::StatTimer StatTimer_main(timer_str.c_str(), regionname);

    StatTimer_main.start();
    SGD::go((*hg), DGAccumulator_accum);
    StatTimer_main.stop();

    if ((run + 1) != numRuns) {
#ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) { 
        //bitset_dist_current_reset_cuda(cuda_ctx);
      } else
#endif
      (*hg).set_num_run(run+1);
      InitializeGraph::go((*hg));
      galois::runtime::getHostBarrier().wait();
    }
  }

  StatTimer_total.stop();


  return 0;
}
