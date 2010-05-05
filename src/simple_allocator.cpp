#include <algorithm>

#include <glog/logging.h>

#include "simple_allocator.hpp"


using std::max;
using std::sort;

using namespace nexus;
using namespace nexus::internal;
using namespace nexus::internal::master;


void SimpleAllocator::frameworkAdded(Framework* framework)
{
  LOG(INFO) << "Added " << framework;
  makeNewOffers();
}


void SimpleAllocator::frameworkRemoved(Framework* framework)
{
  LOG(INFO) << "Removed " << framework;
  foreachpair (Slave* s, unordered_set<Framework*>& refs, refusers)
    refs.erase(framework);
  // TODO: Re-offer just the slaves that the framework had tasks on?
  //       Alternatively, comment this out and wait for a timer tick
  //makeNewOffers();
}


void SimpleAllocator::slaveAdded(Slave* slave)
{
  // LOG(INFO) << "Added " << slave;
  refusers[slave] = unordered_set<Framework*>();
  totalResources += slave->resources;
  //makeNewOffers(slave);
}


void SimpleAllocator::slaveRemoved(Slave* slave)
{
  LOG(INFO) << "Removed " << slave;
  totalResources -= slave->resources;
  refusers.erase(slave);
}


void SimpleAllocator::taskRemoved(Task* task, TaskRemovalReason reason)
{
  LOG(INFO) << "Removed " << task;
  // Remove all refusers from this slave since it has more resources free
  Slave* slave = master->lookupSlave(task->slaveId);
  CHECK(slave != 0);
  refusers[slave].clear();
  // Re-offer the resources, unless this task was removed due to a lost
  // slave or a lost framework (in which case we'll get another callback)
  if (reason == TRR_TASK_ENDED || reason == TRR_EXECUTOR_LOST) {
    // TODO: Use a more efficient makeOffers() that re-offers just one slave?
    //makeNewOffers();
  }
}


void SimpleAllocator::offerReturned(SlotOffer* offer,
                                    OfferReturnReason reason,
                                    const vector<SlaveResources>& resLeft)
{
  LOG(INFO) << "Offer returned: " << offer << ", reason = " << reason;
  // If this offer returned due to the framework replying, add it to refusers
  if (reason == ORR_FRAMEWORK_REPLIED) {
    Framework* framework = master->lookupFramework(offer->frameworkId);
    CHECK(framework != 0);
    foreach (const SlaveResources& r, resLeft) {
      // LOG(INFO) << "Framework reply leaves " << r.resources 
      //           << " free on " << r.slave;
      if (r.resources.cpus > 0 || r.resources.mem > 0) {
        // LOG(INFO) << "Inserting " << framework << " as refuser for " << r.slave
	  ;
        refusers[r.slave].insert(framework);
      }
    }
  }
  // Make new offers, unless the offer returned due to a lost framework or slave
  // (in those cases, frameworkRemoved and slaveRemoved will be called later)
  if (reason != ORR_SLAVE_LOST && reason != ORR_FRAMEWORK_LOST) {
    vector<Slave*> slaves;
    foreach (const SlaveResources& r, resLeft)
      slaves.push_back(r.slave);
    //makeNewOffers(slaves);
  }
}


void SimpleAllocator::offersRevived(Framework* framework)
{
  LOG(INFO) << "Filters removed for " << framework;
  //makeNewOffers();
}


void SimpleAllocator::timerTick()
{
  // TODO: Is this necessary?
  makeNewOffers();
}


namespace {
  
struct DominantShareComparator
{
  Resources total;
  
  DominantShareComparator(Resources _total) : total(_total)
  {
    if (total.cpus == 0) // Prevent division by zero if there are no slaves
      total.cpus = 1;
    if (total.mem == 0)
      total.mem = 1;
  }
  
  bool operator() (Framework* f1, Framework* f2)
  {
    double f1_cpus = f1->resources.cpus;
    double f1_mem = f1->resources.mem;
    foreach (SlotOffer* offer, f1->slotOffers) {
      foreach (const SlaveResources& slaveResources, offer->resources) {
	f1_cpus += slaveResources.resources.cpus;
	f1_mem += slaveResources.resources.mem;
      }
    }

    double f2_cpus = f2->resources.cpus;
    double f2_mem = f2->resources.mem;
    foreach (SlotOffer* offer, f2->slotOffers) {
      foreach (const SlaveResources& slaveResources, offer->resources) {
	f2_cpus += slaveResources.resources.cpus;
	f2_mem += slaveResources.resources.mem;
      }
    }

    // An ordering that only takes into account running tasks.
    // double share1 = max(f1_cpus / (double) total.cpus,
    //                     f1_mem  / (double) total.mem);
    // double share2 = max(f2_cpus / (double) total.cpus,
    //                     f2_mem  / (double) total.mem);

    // An ordering that only takes into account both running tasks and offers.
    double share1 = max(f1->resources.cpus / (double) total.cpus,
                        f1->resources.mem  / (double) total.mem);
    double share2 = max(f2->resources.cpus / (double) total.cpus,
                        f2->resources.mem  / (double) total.mem);

    if (share1 == share2)
      return f1->id < f2->id; // Make the sort deterministic for unit testing
    else
      return share1 < share2;
  }
};

}


vector<Framework*> SimpleAllocator::getAllocationOrdering()
{
  vector<Framework*> frameworks = master->getActiveFrameworks();
  DominantShareComparator comp(totalResources);
  sort(frameworks.begin(), frameworks.end(), comp);
  return frameworks;
}


void SimpleAllocator::makeNewOffers()
{
  // TODO: Create a method in master so that we don't return the whole list of slaves
  vector<Slave*> slaves = master->getActiveSlaves();
  makeNewOffers(slaves);
}


void SimpleAllocator::makeNewOffers(Slave* slave)
{
  vector<Slave*> slaves;
  slaves.push_back(slave);
  makeNewOffers(slaves);
}


void SimpleAllocator::makeNewOffers(const vector<Slave*>& slaves)
{
  LOG(INFO) << "Running makeNewOffers...";
  
  // Get an ordering of frameworks to send offers to
  vector<Framework*> ordering = getAllocationOrdering();
  if (ordering.size() == 0)
    return;
  
  // Find all the free resources that can be allocated
  unordered_map<Slave* , Resources> freeResources;
  foreach (Slave* slave, slaves) {
    if (slave->active) {
      Resources res = slave->resourcesFree();
      if (res.cpus >= MIN_CPUS && res.mem >= MIN_MEM) {
        VLOG(1) << "Found free resources: " << res << " on " << slave;
        freeResources[slave] = res;
      }
    }
  }

  if (freeResources.size() == 0)
    return;
  
  // Clear refusers on any slave that has been refused by everyone
  foreachpair (Slave* slave, _, freeResources) {
    unordered_set<Framework*>& refs = refusers[slave];
    if (refs.size() == ordering.size()) {
      VLOG(1) << "Clearing refusers for " << slave
              << " because everyone refused it";
      refs.clear();
    }
  }

  unordered_map<Framework*, vector<SlaveResources> > offerings;

  foreachpair (Slave* slave, Resources resources, freeResources) {
    foreach (Framework* framework, ordering) {
      // See which resources this framework can take (given filters & refusals)
      if (refusers[slave].find(framework) == refusers[slave].end() &&
          !framework->filters(slave, resources)) {
        VLOG(1) << "Offering " << resources << " on " << slave
                << " to framework " << framework->id;
  	offerings[framework].push_back(SlaveResources(slave, resources));
      }
      // Send out a batch offers if there are at least 100.
      if (offerings[framework].size() == 100) {
  	master->makeOffer(framework, offerings[framework]);
  	offerings[framework].clear();
      }
    }
    ordering = getAllocationOrdering();
  }

  // Offer batch of remaining resources for each framework.
  foreachpair (Framework* fw, vector<SlaveResources>& remaining, offerings) {
    master->makeOffer(fw, remaining);
  }
  
  // foreach (Framework* framework, ordering) {
  //   See which resources this framework can take (given filters & refusals)
  //   vector<SlaveResources> offerable;
  //   foreachpair (Slave* slave, Resources resources, freeResources) {
  //     if (refusers[slave].find(framework) == refusers[slave].end() &&
  //         !framework->filters(slave, resources)) {
  //       VLOG(1) << "Offering " << resources << " on " << slave
  //               << " to framework " << framework->id;
  //       offerable.push_back(SlaveResources(slave, resources));
  //     }
  //     if (offerable.size() >= 100)
  // 	break;
  //   }
  //   if (offerable.size() > 0) {
  //     foreach (SlaveResources& r, offerable) {
  //       freeResources.erase(r.slave);
  //     }
  //     master->makeOffer(framework, offerable);
  //   }
  // }
}
