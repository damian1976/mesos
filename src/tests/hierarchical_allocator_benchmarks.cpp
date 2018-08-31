// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/allocator/allocator.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/queue.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/stopwatch.hpp>

#include "master/constants.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "tests/allocator.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::MIN_CPUS;
using mesos::internal::master::MIN_MEM;

using mesos::internal::master::allocator::HierarchicalDRFAllocator;

using mesos::internal::slave::AGENT_CAPABILITIES;

using mesos::allocator::Allocator;

using process::Clock;
using process::Future;

using std::cout;
using std::endl;
using std::make_shared;
using std::ostream;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// TODO(kapil): Add support for per-framework-profile cofiguration for
// offer acceptance/rejection.
struct FrameworkProfile
{
  FrameworkProfile(const string& _name,
                   const set<string>& _roles,
                   size_t _instances,
                   size_t _maxTasksPerInstance,
                   const Resources& _taskResources,
                   size_t _maxTasksPerOffer)
    : name(_name),
      roles(_roles),
      instances(_instances),
      maxTasksPerInstance(_maxTasksPerInstance),
      taskResources(_taskResources),
      maxTasksPerOffer(_maxTasksPerOffer) {}

  string name;
  set<string> roles;
  size_t instances;

  const size_t maxTasksPerInstance;
  Resources taskResources;
  const size_t maxTasksPerOffer;
};


struct AgentProfile
{
  AgentProfile(const string& _name,
               size_t _instances,
               const Resources& _resources,
               const hashmap<FrameworkID, Resources>& _usedResources)
    : name(_name),
      instances(_instances),
      resources(_resources),
      usedResources(_usedResources) {}

  string name;
  size_t instances;
  Resources resources;
  hashmap<FrameworkID, Resources> usedResources;
};


struct OfferedResources
{
  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources resources;
  string role;
};


struct BenchmarkConfig
{
  BenchmarkConfig(const string& allocator_ = master::DEFAULT_ALLOCATOR,
                  const string& roleSorter_ = "drf",
                  const string& frameworkSorter_ = "drf",
                  const Duration& allocationInterval_ =
                    master::DEFAULT_ALLOCATION_INTERVAL)
    : allocator(allocator_),
      roleSorter(roleSorter_),
      frameworkSorter(frameworkSorter_),
      allocationInterval(allocationInterval_)
  {
    minAllocatableResources.push_back(
        CHECK_NOTERROR(Resources::parse("cpus:" + stringify(MIN_CPUS))));
    minAllocatableResources.push_back(CHECK_NOTERROR(Resources::parse(
        "mem:" + stringify((double)MIN_MEM.bytes() / Bytes::MEGABYTES))));
  }

  string allocator;
  string roleSorter;
  string frameworkSorter;

  Duration allocationInterval;

  vector<Resources> minAllocatableResources;

  vector<FrameworkProfile> frameworkProfiles;
  vector<AgentProfile> agentProfiles;
};


class HierarchicalAllocations_BENCHMARK_TestBase : public ::testing::Test
{
protected:
  HierarchicalAllocations_BENCHMARK_TestBase ()
    : totalTasksToLaunch(0) {}

  ~HierarchicalAllocations_BENCHMARK_TestBase () override
  {
    delete allocator;
  }

  void initializeCluster(
      const BenchmarkConfig& config,
      Option<lambda::function<
          void(const FrameworkID&,
               const hashmap<string, hashmap<SlaveID, Resources>>&)>>
                 offerCallback = None())
  {
    bool clockPaused = Clock::paused();

    // If clock was not paused, pause the clock so that we could
    // make measurements.
    if (!clockPaused) {
      Clock::pause();
    }

    if (offerCallback.isNone()) {
      offerCallback =
        [this](
            const FrameworkID& frameworkId,
            const hashmap<string, hashmap<SlaveID, Resources>>& resources_) {
          foreachkey (const string& role, resources_) {
            foreachpair (
                const SlaveID& slaveId,
                const Resources& resources,
                resources_.at(role)) {
              offers.put(
                  OfferedResources{frameworkId, slaveId, resources, role});
            }
          }
        };
    }

    allocator = CHECK_NOTERROR(Allocator::create(
        config.allocator, config.roleSorter, config.frameworkSorter));

    allocator->initialize(
        config.allocationInterval,
        CHECK_NOTNONE(offerCallback),
        {},
        None(),
        true,
        None(),
        config.minAllocatableResources);

    Stopwatch watch;
    watch.start();

    // Add agents.
    size_t agentCount = 0;
    for (const AgentProfile& profile : config.agentProfiles) {
      agentCount += profile.instances;
      for (size_t i = 0; i < profile.instances; i++) {
        const string agentName = profile.name + "-" + stringify(i);

        SlaveInfo agent;
        *(agent.mutable_resources()) = profile.resources;
        agent.mutable_id()->set_value(agentName);
        agent.set_hostname(agentName);

        allocator->addSlave(
            agent.id(),
            agent,
            AGENT_CAPABILITIES(),
            None(),
            agent.resources(),
            profile.usedResources);
      }
    }

    // Wait for all the `addSlave` operations to be processed.
    Clock::settle();

    watch.stop();

    cout << "Added " << agentCount << " agents"
         << " in " << watch.elapsed() << endl;

    // Pause the allocator here to prevent any event-driven allocations while
    // adding frameworks.
    allocator->pause();

    watch.start();

    // Add frameworks.
    size_t frameworkCount = 0;
    for (const FrameworkProfile& profile : config.frameworkProfiles) {
      totalTasksToLaunch += profile.instances * profile.maxTasksPerInstance;
      frameworkCount += profile.instances;

      shared_ptr<FrameworkProfile> sharedProfile =
        make_shared<FrameworkProfile>(profile);

      for (size_t i = 0; i < profile.instances; i++) {
        const string frameworkName = profile.name + "-" + stringify(i);

        FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
        frameworkInfo.set_name(frameworkName);
        frameworkInfo.mutable_id()->set_value(frameworkName);

        frameworkInfo.clear_roles();

        for (const string& role : profile.roles) {
          frameworkInfo.add_roles(role);
        }

        frameworkProfiles[frameworkInfo.id()] = sharedProfile;

        allocator->addFramework(
            frameworkInfo.id(),
            frameworkInfo,
            {},
            true,
            {});
      }
    }

    // Wait for all the `addFramework` operations to be processed.
    Clock::settle();

    watch.stop();

    cout << "Added " << frameworkCount << " frameworks"
         << " in " << watch.elapsed() << endl;

    // Resume the clock if it was not paused.
    if (!clockPaused) {
      Clock::resume();
    }

    allocator->resume();
  }

  const FrameworkProfile& getFrameworkProfile(const FrameworkID& id)
  {
    return *frameworkProfiles[id];
  }

  Allocator* allocator;

  process::Queue<OfferedResources> offers;

  size_t totalTasksToLaunch;

private:
  hashmap<FrameworkID, shared_ptr<FrameworkProfile>> frameworkProfiles;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {
