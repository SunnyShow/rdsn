// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include "replica/replica.h"
#include "replica/replica_context.h"
#include "replica/replica_stub.h"

namespace dsn {
namespace replication {

class replica_split_manager : replica_base
{
public:
    explicit replica_split_manager(replica *r);
    ~replica_split_manager();

    int32_t get_partition_version() const { return _partition_version.load(); }
    gpid get_child_gpid() const { return _child_gpid; }
    void set_child_gpid(gpid pid) { _child_gpid = pid; }

private:
    // parent partition create child
    void on_add_child(const group_check_request &request);

    // child replica initialize config and state info
    void child_init_replica(gpid parent_gpid, dsn::rpc_address primary_address, ballot init_ballot);

    void parent_prepare_states(const std::string &dir);

    // child copy parent prepare list and call child_learn_states
    void child_copy_prepare_list(learn_state lstate,
                                 std::vector<mutation_ptr> mutation_list,
                                 std::vector<std::string> plog_files,
                                 uint64_t total_file_size,
                                 std::shared_ptr<prepare_list> plist);

    // child learn states(including checkpoint, private logs, in-memory mutations)
    void child_learn_states(learn_state lstate,
                            std::vector<mutation_ptr> mutation_list,
                            std::vector<std::string> plog_files,
                            uint64_t total_file_size,
                            decree last_committed_decree);

    // TODO(heyuchen): total_file_size is used for split perf-counter in further pull request
    // Applies mutation logs that were learned from the parent of this child.
    // This stage follows after that child applies the checkpoint of parent, and begins to apply the
    // mutations.
    // \param last_committed_decree: parent's last_committed_decree when the checkpoint was
    // generated.
    error_code child_apply_private_logs(std::vector<std::string> plog_files,
                                        std::vector<mutation_ptr> mutation_list,
                                        uint64_t total_file_size,
                                        decree last_committed_decree);

    // child catch up parent states while executing async learn task
    void child_catch_up_states();

    // child send notification to primary parent when it finish async learn
    void child_notify_catch_up();

    // primary parent handle child catch_up request
    void parent_handle_child_catch_up(const notify_catch_up_request &request,
                                      notify_cacth_up_response &response);

    // primary parent check if sync_point has been committed
    // sync_point is the first decree after parent send write request to child synchronously
    void parent_check_sync_point_commit(decree sync_point);

    // primary parent register children on meta_server
    void register_child_on_meta(ballot b);
    void on_register_child_on_meta_reply(dsn::error_code ec,
                                         const register_child_request &request,
                                         const register_child_response &response);
    // primary sends register request to meta_server
    void parent_send_register_request(const register_child_request &request);

    // child partition has been registered on meta_server, could be active
    void child_partition_active(const partition_configuration &config);

    // return true if parent status is valid
    bool parent_check_states();

    // parent reset child information when partition split failed
    void parent_cleanup_split_context();
    // child suicide when partition split failed
    void child_handle_split_error(const std::string &error_msg);
    // child handle error while async learn parent states
    void child_handle_async_learn_error();

    //
    // helper functions
    //
    partition_status::type status() const { return _replica->status(); }
    ballot get_ballot() const { return _replica->get_ballot(); }
    decree last_committed_decree() const { return _replica->last_committed_decree(); }
    task_tracker *tracker() { return _replica->tracker(); }

private:
    replica *_replica;
    replica_stub *_stub;

    friend class replica;
    friend class replica_stub;
    friend class replica_split_test;

    // _child_gpid = gpid({app_id},{pidx}+{old_partition_count}) for parent partition
    // _child_gpid.app_id = 0 for parent partition not in partition split and child partition
    dsn::gpid _child_gpid{0, 0};
    // ballot when starting partition split and split will stop if ballot changed
    // _child_init_ballot = 0 if partition not in partition split
    ballot _child_init_ballot{0};
    // in normal cases, _partition_version = partition_count-1
    // when replica reject client read write request, partition_version = -1
    std::atomic<int32_t> _partition_version;
};

} // namespace replication
} // namespace dsn
