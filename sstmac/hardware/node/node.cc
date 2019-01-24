/**
Copyright 2009-2018 National Technology and Engineering Solutions of Sandia, 
LLC (NTESS).  Under the terms of Contract DE-NA-0003525, the U.S.  Government 
retains certain rights in this software.

Sandia National Laboratories is a multimission laboratory managed and operated
by National Technology and Engineering Solutions of Sandia, LLC., a wholly 
owned subsidiary of Honeywell International, Inc., for the U.S. Department of 
Energy's National Nuclear Security Administration under contract DE-NA0003525.

Copyright (c) 2009-2018, NTESS

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, 
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Questions? Contact sst-macro-help@sandia.gov
*/

#include <sstmac/software/libraries/unblock_event.h>
#include <sstmac/software/process/operating_system.h>
#include <sstmac/hardware/node/node.h>
#include <sstmac/hardware/nic/nic.h>
#include <sstmac/hardware/memory/memory_model.h>
#include <sstmac/hardware/processor/processor.h>
#include <sstmac/hardware/interconnect/interconnect.h>
#include <sstmac/software/process/operating_system.h>
#include <sstmac/software/process/app.h>
#include <sstmac/software/launch/app_launcher.h>
#include <sstmac/software/launch/launch_event.h>
#include <sstmac/software/launch/job_launcher.h>
#include <sstmac/common/event_callback.h>
#include <sstmac/common/runtime.h>
#include <sprockit/keyword_registration.h>
#include <sprockit/sim_parameters.h>
#include <sprockit/util.h>
#include <sprockit/output.h>

#include <sstmac/sst_core/integrated_component.h>
#include <sstmac/sst_core/connectable_wrapper.h>

RegisterDebugSlot(node)
RegisterNamespaces("os", "memory", "proc", "node");
RegisterKeywords(
{ "nsockets", "the number of sockets/processors in a node" },
{ "node_name", "DEPRECATED: the type of node on each endpoint" },
{ "node_MemoryModel", "DEPRECATED: the type of memory model on each node" },
{ "node_sockets", "DEPRECATED: the number of sockets/processors in a node" },
{ "JobLauncher", "the type of launcher for scheduling jobs on the system - equivalent to MOAB or SLURM" },
);

namespace sstmac {
namespace hw {

using namespace sstmac::sw;

Node::Node(SST::Params& params, uint32_t id)
  : ConnectableComponent(params, id),
  params_(params),
  app_refcount_(0),
  job_launcher_(nullptr)
{
#if SSTMAC_INTEGRATED_SST_CORE
  static bool init_debug = false;
  if (!init_debug){
    std::vector<std::string> debug_params;
    if (params.contains("debug")){
      params.find_array("debug", debug_params);
    }
    for (auto& str : debug_params){
      sprockit::debug::turn_on(str);
    }
    init_debug = true;
  }
#endif
  my_addr_ = params.find<int>("id");
  next_outgoing_id_.setSrcNode(my_addr_);

  SST::Params nic_params = params.find_prefix_params("nic");
  nic_ = NIC::factory::get_param("name", nic_params, this);
  nic_params->remove_param("id");

  SST::Params mem_params = params.find_prefix_params("memory");
  mem_model_ = MemoryModel::factory::get_optional_param("name", "logp", mem_params, this);

  SST::Params proc_params = params.find_prefix_params("proc");
  proc_ = Processor::factory::get_optional_param("processor", "instruction",
          proc_params,
          mem_model_, this);

  nsocket_ = params.find<int>("nsockets", 1);

  SST::Params os_params = params.find_prefix_params("os");
  os_ = new sw::OperatingSystem(os_params, this);

  app_launcher_ = new AppLauncher(os_);

  launchRoot_ = params.find<int>("launchRoot", 0);
  if (my_addr_ == launchRoot_){
    job_launcher_ =   JobLauncher::factory::get_optional_param(
          "JobLauncher", "default", params, os_);
  }
}

LinkHandler*
Node::creditHandler(int port)
{
  return nic_->creditHandler(port);
}

LinkHandler*
Node::payloadHandler(int port)
{
  return nic_->payloadHandler(port);
}

void
Node::setup()
{
  Component::setup();
  mem_model_->setup();
  os_->setup();
  nic_->setup();
  if (job_launcher_)
    job_launcher_->scheduleLaunchRequests();
}

void
Node::init(unsigned int phase)
{
#if SSTMAC_INTEGRATED_SST_CORE
  Component::init(phase);
#endif
  nic_->init(phase);
  os_->init(phase);
  mem_model_->init(phase);
}

Node::~Node()
{
  if (job_launcher_) delete job_launcher_;
  if (app_launcher_) delete app_launcher_;
  if (mem_model_) delete mem_model_;
  if (proc_) delete proc_;
  if (os_) delete os_;
  if (nic_) delete nic_;
  //if (JobLauncher_) delete JobLauncher_;
}

void
Node::connectOutput(SST::Params& params,
  int src_outport, int dst_inport,
  EventLink* link)
{
  //forward connection to nic
  nic_->connectOutput(params, src_outport, dst_inport, link);
}

void
Node::connectInput(SST::Params& params,
  int src_outport, int dst_inport,
  EventLink* link)
{
  //forward connection to nic
  nic_->connectInput(params/*.find_prefix_params("nic")*/, src_outport, dst_inport, link);
}

Timestamp
Node::sendLatency(SST::Params& params) const
{
  auto nic_params = params.find_prefix_params("nic");
  return nic_->sendLatency(nic_params);
}

Timestamp
Node::creditLatency(SST::Params& params) const
{
  auto nic_params = params.find_prefix_params("nic");
  return nic_->creditLatency(nic_params);
}

void
Node::execute(ami::SERVICE_FUNC func, Event* data)
{
  sprockit::abort("node does not implement asynchronous services - choose new node model");
}

std::string
Node::toString() const
{
  return sprockit::printf("node(%d)", int(my_addr_));
}

void
Node::incrementAppRefcount()
{
#if SSTMAC_INTEGRATED_SST_CORE
  if (app_refcount_ == 0){
    primaryComponentDoNotEndSim();
  }
#endif
  ++app_refcount_;
}

void
Node::decrementAppRefcount()
{
  app_refcount_--;

#if SSTMAC_INTEGRATED_SST_CORE
  if (app_refcount_ == 0){
    primaryComponentOKToEndSim();
  }
#endif
}

void
Node::handle(Event* ev)
{
  os_->handleEvent(ev);
}

}
} // end of namespace sstmac
