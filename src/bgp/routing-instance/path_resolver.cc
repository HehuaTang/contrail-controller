/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/routing-instance/path_resolver.h"

#include "base/lifetime.h"
#include "base/task.h"
#include "base/task_annotations.h"
#include "base/task_trigger.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_table.h"

using std::make_pair;
using std::string;
using std::vector;

class PathResolver::DeleteActor : public LifetimeActor {
public:
    explicit DeleteActor(PathResolver *resolver)
        : LifetimeActor(resolver->table()->server()->lifetime_manager()),
          resolver_(resolver) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return resolver_->MayDelete();
    }

    virtual void Destroy() {
        resolver_->table()->DestroyPathResolver();
    }

private:
    PathResolver *resolver_;
};

//
// Constructor for PathResolver.
//
// A new PathResolver is created from BgpTable::CreatePathResolver for inet
// and inet6 tables in all non-default RoutingInstances.
//
// The listener_id if used to set state on BgpRoutes for BgpPaths that have
// requested resolution.
//
PathResolver::PathResolver(BgpTable *table)
    : table_(table),
      condition_listener_(table->server()->condition_listener(table->family())),
      listener_id_(table->Register(
          boost::bind(&PathResolver::RouteListener, this, _1, _2),
          "PathResolver")),
      nexthop_reg_unreg_trigger_(new TaskTrigger(
          boost::bind(&PathResolver::ProcessResolverNexthopRegUnregList, this),
          TaskScheduler::GetInstance()->GetTaskId("bgp::Config"),
          0)),
      nexthop_update_trigger_(new TaskTrigger(
          boost::bind(&PathResolver::ProcessResolverNexthopUpdateList, this),
          TaskScheduler::GetInstance()->GetTaskId("bgp::ResolverNexthop"),
          0)),
      deleter_(new DeleteActor(this)),
      table_delete_ref_(this, table->deleter()) {
    for (int part_id = 0; part_id < DB::PartitionCount(); ++part_id) {
        partitions_.push_back(new PathResolverPartition(part_id, this));
    }
}

//
// Destructor for PathResolver.
//
// A PathResolver is deleted via LifetimeManager deletion.
// Actual destruction of the object happens via BgpTable::DestroyPathResolver.
//
// Need to do a deep delete of the partitions vector to ensure deletion of all
// PathResolverPartitions.
//
PathResolver::~PathResolver() {
    assert(listener_id_ != DBTableBase::kInvalidId);
    table_->Unregister(listener_id_);
    STLDeleteValues(&partitions_);
}

//
// Get the address family for PathResolver.
//
Address::Family PathResolver::family() const {
    return table_->family();
}

//
// Request PathResolver to start resolution for the given BgpPath.
// This API needs to be called explicitly when the BgpPath needs resolution.
// This is typically when the BgpPath is added, but may also be needed when
// the BgpPath changes nexthop.
//
void PathResolver::StartPathResolution(int part_id, const BgpPath *path,
    BgpRoute *route) {
    CHECK_CONCURRENCY("db::DBTable");

    partitions_[part_id]->StartPathResolution(path, route);
}

//
// Request PathResolver to update resolution for the given BgpPath.
// This API needs to be called explicitly when a BgpPath needing resolution
// gets updated with new attributes. Note that nexthop change could require
// the caller to call StartPathResolution instead.
//
void PathResolver::UpdatePathResolution(int part_id, const BgpPath *path) {
    CHECK_CONCURRENCY("db::DBTable");

    partitions_[part_id]->UpdatePathResolution(path);
}

//
// Request PathResolver to stop resolution for the given BgpPath.
// This API needs to be called explicitly when the BgpPath does not require
// resolution. This is typically when the BgpPath is deleted, but may also be
// needed when the BgpPath changes nexthop.
//
void PathResolver::StopPathResolution(int part_id, const BgpPath *path) {
    CHECK_CONCURRENCY("db::DBTable");

    partitions_[part_id]->StopPathResolution(path);
}

//
// Add a ResolverNexthop to the register/unregister list and start the Task
// to process the list.
//
// Note that the operation (register/unregister) is not explicitly part of
// the list - it's inferred based on the state of the ResolverNexthop when
// the list is processed.
//
void PathResolver::RegisterUnregisterResolverNexthop(
    ResolverNexthop *rnexthop) {
    tbb::mutex::scoped_lock lock(mutex_);
    nexthop_reg_unreg_list_.insert(rnexthop);
    nexthop_reg_unreg_trigger_->Set();
}

//
// Add a ResolverNexthop to the update list and start the Task to process the
// list.
//
void PathResolver::UpdateResolverNexthop(ResolverNexthop *rnexthop) {
    tbb::mutex::scoped_lock lock(mutex_);
    nexthop_update_list_.insert(rnexthop);
    nexthop_update_trigger_->Set();
}

//
// Get the PathResolverPartition for the given part_id.
//
PathResolverPartition *PathResolver::GetPartition(int part_id) {
    return partitions_[part_id];
}

//
// Find or create the ResolverNexthop with the given IpAddress.
// Called when a new ResolverPath is being created.
//
// A newly created ResolverNexthop is added to the map.
//
ResolverNexthop *PathResolver::LocateResolverNexthop(IpAddress address) {
    CHECK_CONCURRENCY("db::DBTable");

    tbb::mutex::scoped_lock lock(mutex_);
    ResolverNexthopMap::iterator loc = nexthop_map_.find(address);
    if (loc != nexthop_map_.end()) {
        return loc->second;
    } else {
        ResolverNexthop *rnexthop = new ResolverNexthop(this, address);
        nexthop_map_.insert(make_pair(address, rnexthop));
        return rnexthop;
    }
}

//
// Remove the ResolverNexthop from the map and the update list.
// Called when ResolverPath is being unregistered from BgpConditionListener
// as part of register/unregister list processing.
//
// If the ResolverNexthop is being unregistered, it's moved to the delete
// list till the BgpConditionListener invokes the remove complete callback.
//
// Note that a ResolverNexthop object cannot be resurrected once it has been
// removed from the map - it's destined to get destroyed eventually. A new
// object for the same IpAddress gets created if a new ResolverPath needs to
// use one.
//
void PathResolver::RemoveResolverNexthop(ResolverNexthop *rnexthop) {
    CHECK_CONCURRENCY("bgp::Config");

    ResolverNexthopMap::iterator loc = nexthop_map_.find(rnexthop->address());
    assert(loc != nexthop_map_.end());
    nexthop_map_.erase(loc);
    nexthop_update_list_.erase(rnexthop);
    if (rnexthop->registered())
        nexthop_delete_list_.insert(rnexthop);
}

//
// Callback for ConditionMatch remove operation to BgpConditionListener.
//
// It's safe to destroy the ResolverNexthop at this point. It may also now be
// feasible to go ahead and proceed with deletion of the PathResolver itself
// if this was the last ResolverNexthop pending removal.
//
void PathResolver::UnregisterResolverNexthopDone(BgpTable *table,
    ConditionMatch *match) {
    CHECK_CONCURRENCY("db::DBTable");

    ResolverNexthop *rnexthop = dynamic_cast<ResolverNexthop *>(match);
    assert(rnexthop);
    assert(!rnexthop->registered());

    tbb::mutex::scoped_lock lock(mutex_);
    nexthop_delete_list_.erase(rnexthop);
    if (MayDelete())
        RetryDelete();
    delete rnexthop;
}

//
// Handle processing of a ResolverNexthop on the register/unregister list.
//
// Return true if the ResolverNexthop can be deleted immediately.
//
bool PathResolver::ProcessResolverNexthopRegUnreg(ResolverNexthop *rnexthop) {
    CHECK_CONCURRENCY("bgp::Config");

    if (rnexthop->registered()) {
        // Unregister the ResolverNexthop from BgpConditionListener if there
        // are no more ResolverPaths using it.
        if (rnexthop->empty()) {
            RemoveResolverNexthop(rnexthop);
            BgpConditionListener::RequestDoneCb cb = boost::bind(
                &PathResolver::UnregisterResolverNexthopDone, this, _1, _2);
            condition_listener_->RemoveMatchCondition(table_, rnexthop, cb);
            rnexthop->clear_registered();
        }
    } else {
        // Register the ResolverNexthop if there's at least one ResolverPath
        // using it. It can be deleted right away if there's no ResolverPaths
        // using it.
        if (!rnexthop->empty()) {
            condition_listener_->AddMatchCondition(
                table_, rnexthop, BgpConditionListener::RequestDoneCb());
            rnexthop->set_registered();
        } else {
            RemoveResolverNexthop(rnexthop);
            return true;
        }
    }

    return false;
}

//
// Handle processing of all ResolverNexthops on the register/unregister list.
//
bool PathResolver::ProcessResolverNexthopRegUnregList() {
    CHECK_CONCURRENCY("bgp::Config");

    for (ResolverNexthopList::iterator it = nexthop_reg_unreg_list_.begin();
         it != nexthop_reg_unreg_list_.end(); ++it) {
        ResolverNexthop *rnexthop = *it;
        if (ProcessResolverNexthopRegUnreg(rnexthop))
            delete rnexthop;
    }
    nexthop_reg_unreg_list_.clear();

    if (MayDelete())
        RetryDelete();
    return true;
}

//
// Handle processing of all ResolverNexthops on the update list.
//
bool PathResolver::ProcessResolverNexthopUpdateList() {
    CHECK_CONCURRENCY("bgp::ResolverNexthop");

    for (ResolverNexthopList::iterator it = nexthop_update_list_.begin();
         it != nexthop_update_list_.end(); ++it) {
        ResolverNexthop *rnexthop = *it;
        rnexthop->TriggerAllResolverPaths();
    }
    nexthop_update_list_.clear();
    return true;
}

//
// Return true if the DeleteActor is marked deleted.
//
bool PathResolver::IsDeleted() const {
    return deleter_->IsDeleted();
}

//
// Cascade delete from BgpTable delete_ref to self.
//
void PathResolver::ManagedDelete() {
    deleter_->Delete();
}

//
// Return true if it's safe to delete the PathResolver.
//
bool PathResolver::MayDelete() const {
    if (!nexthop_map_.empty())
        return false;
    if (!nexthop_delete_list_.empty())
        return false;
    assert(nexthop_reg_unreg_list_.empty());
    assert(nexthop_update_list_.empty());
    return true;
}

//
// Attempt to enqueue a delete for the PathResolver.
//
void PathResolver::RetryDelete() {
    if (!deleter_->IsDeleted())
        return;
    deleter_->RetryDelete();
}

// nsheth: need to find a way to register a listener without a callback and
// remove this method.
bool PathResolver::RouteListener(DBTablePartBase *root, DBEntryBase *entry) {
    return true;
}

//
// Constructor for PathResolverPartition.
// A new PathResolverPartition is created when a PathResolver is created.
//
PathResolverPartition::PathResolverPartition(int part_id,
    PathResolver *resolver)
    : part_id_(part_id),
      resolver_(resolver),
      rpath_update_trigger_(new TaskTrigger(
          boost::bind(&PathResolverPartition::ProcessResolverPathUpdateList,
              this),
          TaskScheduler::GetInstance()->GetTaskId("bgp::ResolverPath"),
          part_id)) {
}

//
// Destructor for PathResolverPartition.
// All PathResolverPartitions for a PathResolver are destroyed when the
// PathResolver is destroyed.
//
PathResolverPartition::~PathResolverPartition() {
}

//
// Start resolution for the given BgpPath.
// Create a ResolverPath object and trigger resolution for it.
// A ResolverNexthop is also created if required.
//
// Note that the ResolverPath gets linked to the ResolverNexthop via the
// ResolverPath constructor.
//
void PathResolverPartition::StartPathResolution(const BgpPath *path,
    BgpRoute *route) {
    IpAddress address = path->GetAttr()->nexthop();
    ResolverNexthop *rnexthop = resolver_->LocateResolverNexthop(address);
    assert(!FindResolverPath(path));
    ResolverPath *rpath = CreateResolverPath(path, route, rnexthop);
    TriggerPathResolution(rpath);
}

//
// Update resolution for the given BgpPath.
// A change in the ResolverNexthop is handled by triggering deletion of the
// old ResolverPath and creating a new one.
//
void PathResolverPartition::UpdatePathResolution(const BgpPath *path) {
    ResolverPath *rpath = FindResolverPath(path);
    assert(rpath);
    const ResolverNexthop *rnexthop = rpath->rnexthop();
    if (rnexthop->address() != path->GetAttr()->nexthop()) {
        StopPathResolution(path);
        StartPathResolution(path, rpath->route());
    } else {
        TriggerPathResolution(rpath);
    }
}

//
// Stop resolution for the given BgpPath.
// The ResolverPath is removed from the map right away, but the deletion of
// any resolved BgpPaths and the ResolverPath itself happens asynchronously.
//
void PathResolverPartition::StopPathResolution(const BgpPath *path) {
    ResolverPath *rpath = RemoveResolverPath(path);
    assert(rpath);
    TriggerPathResolution(rpath);
}

//
// Add a ResolverPath to the update list and start Task to process the list.
//
void PathResolverPartition::TriggerPathResolution(ResolverPath *rpath) {
    CHECK_CONCURRENCY("db::DBTable", "bgp::ResolverNexthop");

    rpath_update_list_.insert(rpath);
    rpath_update_trigger_->Set();
}

//
// Create a new ResolverPath for the BgpPath.
// The ResolverPath is inserted into the map.
//
ResolverPath *PathResolverPartition::CreateResolverPath(const BgpPath *path,
    BgpRoute *route, ResolverNexthop *rnexthop) {
    ResolverPath *rpath = new ResolverPath(this, path, route, rnexthop);
    rpath_map_.insert(make_pair(path, rpath));
    return rpath;
}

//
// Find the ResolverPath for given BgpPath.
//
ResolverPath *PathResolverPartition::FindResolverPath(const BgpPath *path) {
    PathToResolverPathMap::iterator loc = rpath_map_.find(path);
    return (loc != rpath_map_.end() ? loc->second : NULL);
}

//
// Remove the ResolverPath for given BgpPath.
// The ResolverPath is removed from the map and it's back pointer to the
// BgpPath is cleared.
// Actual deletion of the ResolverPath happens asynchronously.
//
ResolverPath *PathResolverPartition::RemoveResolverPath(const BgpPath *path) {
    PathToResolverPathMap::iterator loc = rpath_map_.find(path);
    if (loc == rpath_map_.end()) {
        return NULL;
    } else {
        ResolverPath *rpath = loc->second;
        rpath_map_.erase(loc);
        rpath->clear_path();
        return rpath;
    }
}

//
// Handle processing of all ResolverPaths on the update list.
//
bool PathResolverPartition::ProcessResolverPathUpdateList() {
    CHECK_CONCURRENCY("bgp::ResolverPath");

    for (ResolverPathList::iterator it = rpath_update_list_.begin();
         it != rpath_update_list_.end(); ++it) {
        ResolverPath *rpath = *it;
        if (rpath->UpdateResolvedPaths())
            delete rpath;
    }
    rpath_update_list_.clear();
    return true;
}

//
// Constructor for ResolverRouteState.
// Gets called via static method LocateState when the first ResolverPath for
// a BgpRoute is created.
//
// Set State on the BgpRoute to ensure that it doesn't go away.
//
ResolverRouteState::ResolverRouteState(PathResolverPartition *partition,
    BgpRoute *route)
    : partition_(partition),
      route_(route),
      refcount_(0) {
    route_->SetState(partition_->table(), partition_->listener_id(), this);
}

//
// Destructor for ResolverRouteState.
// Gets called via when the refcount goes to 0. This happens when the last
// ResolverPath for a BgpRoute is deleted.
//
// Remove State on the BgpRoute so that deletion can proceed.
//
ResolverRouteState::~ResolverRouteState() {
    route_->ClearState(partition_->table(), partition_->listener_id());
}

//
// Find or create ResolverRouteState for the given BgpRoute.
//
// Note that the refcount for ResolverRouteState gets incremented when the
// ResolverPath takes an intrusive_ptr to it.
//
ResolverRouteState *ResolverRouteState::LocateState(
    PathResolverPartition *partition, BgpRoute *route) {
    ResolverRouteState *state = static_cast<ResolverRouteState *>(
        route->GetState(partition->table(), partition->listener_id()));
    if (state) {
        return state;
    } else {
        return (new ResolverRouteState(partition, route));
    }
}

//
// Constructor for ResolverPath.
// Add the ResolverPath as a dependent of the ResolverNexthop.
//
// Note that it's the caller's responsibility to add the ResolverPath to the
// map in the PathResolverPartition.
//
ResolverPath::ResolverPath(PathResolverPartition *partition,
    const BgpPath *path, BgpRoute *route, ResolverNexthop *rnexthop)
    : partition_(partition),
      path_(path),
      route_(route),
      rnexthop_(rnexthop),
      state_(ResolverRouteState::LocateState(partition, route)) {
    rnexthop->AddResolverPath(partition->part_id(), this);
}

//
// Destructor for ResolverPath.
// Remove the ResolverPath as a dependent of the ResolverNexthop. This may
// trigger unregistration and eventual deletion of the ResolverNexthop if
// there are no more ResolverPaths using it.
//
// Note that the ResolverPath would have been removed from the map in the
// PathResolverPartition much earlier i.e. when resolution is stopped.
//
ResolverPath::~ResolverPath() {
    rnexthop_->RemoveResolverPath(partition_->part_id(), this);
}

//
// Update resolved BgpPaths for the ResolverPath based on the BgpRoute for
// the ResolverNexthop.
//
// Return true if the ResolverPath can be deleted.
//
// Note that the ResolverPath can only be deleted if resolution for it has
// been stooped. It must not be deleted simply because there is no viable
// BgpRoute for the ResolverNexthop.
//
bool ResolverPath::UpdateResolvedPaths() {
    CHECK_CONCURRENCY("bgp::ResolverPath");

    return (path_ == NULL);
}

//
// Constructor for ResolverNexthop.
// Initialize the vector of paths_lists to the number of DB partitions.
//
ResolverNexthop::ResolverNexthop(PathResolver *resolver, IpAddress address)
    : resolver_(resolver),
      address_(address),
      registered_(false),
      route_(NULL),
      rpath_lists_(DB::PartitionCount()) {
}

//
// Destructor for ResolverNexthop.
// A deep delete of the path_lists vector is not required.
//
ResolverNexthop::~ResolverNexthop() {
}

//
// Implement virtual method for ConditionMatch base class.
//
string ResolverNexthop::ToString() const {
    return (string("ResolverNexthop ") + address_.to_string());
}

//
// Implement virtual method for ConditionMatch base class.
//
bool ResolverNexthop::Match(BgpServer *server, BgpTable *table,
    BgpRoute *route, bool deleted) {
    return false;
}

//
// Add the given ResolverPath to the list of dependents in the partition.
// Add to register/unregister list when the first dependent ResolverPath for
// the partition is added.
//
// This may cause the ResolverNexthop to get added to register/unregister
// list multiple times - once for the first ResolverPath in each partition.
// This case is handled by PathResolver::ProcessResolverNexthopRegUnreg.
//
// Do not attempt to access other partitions due to concurrency issues.
//
void ResolverNexthop::AddResolverPath(int part_id, ResolverPath *rpath) {
    CHECK_CONCURRENCY("db::DBTable");

    if (rpath_lists_[part_id].empty())
        resolver_->RegisterUnregisterResolverNexthop(this);
    rpath_lists_[part_id].insert(rpath);
}

//
// Remove given ResolverPath from the list of dependents in the partition.
// Add to register/unregister list when the last dependent ResolverPath for
// the partition is removed.
//
// This may cause the ResolverNexthop to get added to register/unregister
// list multiple times - once for the last ResolverPath in each partition.
// This case is handled by PathResolver::ProcessResolverNexthopRegUnreg.
//
// Do not attempt to access other partitions due to concurrency issues.
//
void ResolverNexthop::RemoveResolverPath(int part_id, ResolverPath *rpath) {
    CHECK_CONCURRENCY("bgp::ResolverPath");

    rpath_lists_[part_id].erase(rpath);
    if (rpath_lists_[part_id].empty())
        resolver_->RegisterUnregisterResolverNexthop(this);
}

//
// Trigger update of resolved BgpPaths for all ResolverPaths that depend on
// the ResolverNexthop. Actual update of the resolved BgpPaths happens when
// the PathResolverPartitions process their update lists.
//
void ResolverNexthop::TriggerAllResolverPaths() const {
    CHECK_CONCURRENCY("bgp::ResolverNexthop");

    for (int part_id = 0; part_id < DB::PartitionCount(); ++part_id) {
        for (ResolverPathList::iterator it = rpath_lists_[part_id].begin();
             it != rpath_lists_[part_id].end(); ++it) {
            ResolverPath *rpath = *it;
            resolver_->GetPartition(part_id)->TriggerPathResolution(rpath);
        }
    }
}

//
// Return true if there are no dependent ResolverPaths in all partitions.
//
bool ResolverNexthop::empty() const {
    CHECK_CONCURRENCY("bgp::Config");

    for (int part_id = 0; part_id < DB::PartitionCount(); ++part_id) {
        if (!rpath_lists_[part_id].empty())
            return false;
    }
    return true;
}
