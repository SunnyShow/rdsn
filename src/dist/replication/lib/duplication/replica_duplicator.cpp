/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "replica_duplicator.h"
#include "load_from_private_log.h"
#include "duplication_pipeline.h"

#include <dsn/dist/replication/replication_app_base.h>
#include <dsn/dist/fmt_logging.h>
#include <rapidjson/writer.h>

namespace dsn {
namespace replication {

replica_duplicator::replica_duplicator(const duplication_entry &ent, replica *r)
    : replica_base(r), _id(ent.dupid), _remote_cluster_address(ent.remote_address), _replica(r)
{
    dassert_replica(ent.status == duplication_status::DS_START ||
                        ent.status == duplication_status::DS_PAUSE,
                    "invalid duplication status: {}",
                    duplication_status_to_string(ent.status));
    _status = ent.status;

    auto it = ent.progress.find(get_gpid().get_partition_index());
    dassert_replica(it != ent.progress.end(), "");
    _progress.last_decree = _progress.confirmed_decree = it->second;
    ddebug_replica(
        "initialize replica_duplicator [dupid:{}, meta_confirmed_decree:{}]", id(), it->second);

    init_metrics_timer();

    /// ===== pipeline declaration ===== ///

    thread_pool(LPC_REPLICATION_LOW).task_tracker(tracker()).thread_hash(get_gpid().thread_hash());

    // load -> ship -> load
    _ship = make_unique<ship_mutation>(this);
    _load_private = make_unique<load_from_private_log>(_replica, this);
    _load = make_unique<load_mutation>(this, _replica, _load_private.get());

    from(*_load).link(*_ship).link(*_load);
    fork(*_load_private, LPC_REPLICATION_LONG_LOW, 0).link(*_ship);

    if (_status == duplication_status::DS_START) {
        start();
    }
}

void replica_duplicator::init_metrics_timer()
{
    constexpr int METRICS_UPDATE_INTERVAL = 10;

    _pending_duplicate_count.init_app_counter(
        "eon.replica",
        fmt::format("dup.pending_duplicate_count@{}", get_gpid()).c_str(),
        COUNTER_TYPE_NUMBER,
        "number of mutations pending for duplication");

    _increased_confirmed_decree.init_app_counter(
        "eon.replica",
        fmt::format("dup.increased_confirmed_decree@{}", get_gpid()).c_str(),
        COUNTER_TYPE_NUMBER,
        fmt::format("number of increased confirmed decree during last {}s", METRICS_UPDATE_INTERVAL)
            .data());

    _last_recorded_confirmed_decree = _progress.confirmed_decree;

    // update the metrics periodically
    _metrics_update_timer = tasking::enqueue_timer(
        LPC_REPLICATION_LOW,
        nullptr, // cancel it manually
        [this]() {
            _pending_duplicate_count->set(_replica->last_committed_decree() -
                                          _progress.confirmed_decree);

            auto p = progress();
            _increased_confirmed_decree->set(p.confirmed_decree - _last_recorded_confirmed_decree);
            _last_recorded_confirmed_decree = p.confirmed_decree;
        },
        METRICS_UPDATE_INTERVAL * 1_s,
        get_gpid().thread_hash());
}

void replica_duplicator::start()
{
    ddebug_replica(
        "starting duplication {} [last_decree: {}, confirmed_decree: {}, max_gced_decree: {}]",
        to_string(),
        _progress.last_decree,
        _progress.confirmed_decree,
        get_max_gced_decree());
    run_pipeline();
}

std::string replica_duplicator::to_string() const
{
    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();

    doc.AddMember("dupid", id(), alloc);
    doc.AddMember("status", rapidjson::StringRef(duplication_status_to_string(_status)), alloc);
    doc.AddMember("remote", rapidjson::StringRef(_remote_cluster_address.data()), alloc);
    doc.AddMember("confirmed", _progress.confirmed_decree, alloc);
    doc.AddMember("app",
                  rapidjson::StringRef(_replica->get_app_info()->app_name.data(),
                                       _replica->get_app_info()->app_name.size()),
                  alloc);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);
    return sb.GetString();
}

void replica_duplicator::update_status_if_needed(duplication_status::type next_status)
{
    if (_status == next_status) {
        return;
    }
    _status = next_status;

    if (next_status == duplication_status::DS_START) {
        start();
    } else if (next_status == duplication_status::DS_PAUSE) {
        ddebug_replica("pausing duplication: {}", to_string());
        pause();
    } else {
        dassert("unexpected duplication status (%s)", duplication_status_to_string(next_status));
    }
}

replica_duplicator::~replica_duplicator()
{
    _metrics_update_timer->cancel(true);

    pause();
    wait_all();
    ddebug_replica("Closing duplication {}", to_string());

    _pending_duplicate_count.clear();
}

void replica_duplicator::update_progress(const duplication_progress &p)
{
    zauto_write_lock l(_lock);

    dassert_replica(p.confirmed_decree <= 0 || _progress.confirmed_decree <= p.confirmed_decree,
                    "never decrease confirmed_decree: new({}) old({})",
                    p.confirmed_decree,
                    _progress.confirmed_decree);

    _progress.confirmed_decree = std::max(_progress.confirmed_decree, p.confirmed_decree);
    _progress.last_decree = std::max(_progress.last_decree, p.last_decree);

    dassert_replica(_progress.confirmed_decree <= _progress.last_decree,
                    "last_decree({}) should always larger than confirmed_decree({})",
                    _progress.last_decree,
                    _progress.confirmed_decree);
}

error_s replica_duplicator::verify_start_decree(decree start_decree)
{
    decree confirmed_decree = progress().confirmed_decree;
    decree last_decree = progress().last_decree;
    decree max_gced_decree = get_max_gced_decree();
    if (max_gced_decree >= start_decree) {
        return error_s::make(
            ERR_CORRUPTION,
            fmt::format(
                "the logs haven't yet duplicated were accidentally truncated "
                "[max_gced_decree: {}, start_decree: {}, confirmed_decree: {}, last_decree: {}]",
                max_gced_decree,
                start_decree,
                confirmed_decree,
                last_decree));
    }
    return error_s::ok();
}

decree replica_duplicator::get_max_gced_decree() const
{
    return _replica->private_log()->max_gced_decree(_replica->get_gpid());
}

} // namespace replication
} // namespace dsn