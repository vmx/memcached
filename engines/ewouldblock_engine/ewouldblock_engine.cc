/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/*
 *                "ewouldblock_engine"
 *
 * The "ewouldblock_engine" allows one to test how memcached responds when the
 * engine returns EWOULDBLOCK instead of the correct response.
 *
 * Motivation:
 *
 * The EWOULDBLOCK response code can be returned from a number of engine
 * functions, and is used to indicate that the request could not be immediately
 * fulfilled, and it "would block" if it tried to. The correct way for
 * memcached to handle this (in general) is to suspend that request until it
 * is later notified by the engine (via notify_io_complete()).
 *
 * However, engines typically return the correct response to requests
 * immediately, only rarely (and from memcached's POV non-deterministically)
 * returning EWOULDBLOCK. This makes testing of the code-paths handling
 * EWOULDBLOCK tricky.
 *
 *
 * Operation:
 * This engine, when loaded by memcached proxies requests to a "real" engine.
 * Depending on how it is configured, it can simply pass the request on to the
 * real engine, or artificially return EWOULDBLOCK back to memcached.
 *
 * See the 'Modes' enum below for the possible modes. The mode can be selected
 * by sending a `request_ewouldblock_ctl` command
 *  (opcode PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL).
 */

#include "config.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>

#include <memcached/engine.h>
#include "utilities/engine_loader.h"

// Shared state between the main thread of execution and the background
// thread processing pending io ops.
static std::mutex mutex;
static std::condition_variable condvar;
static std::queue<const void*> pending_io_ops;
std::atomic_bool stop_notification_thread;


/* Public API declaration ****************************************************/

extern "C" {
    MEMCACHED_PUBLIC_API
    ENGINE_ERROR_CODE create_instance(uint64_t interface, GET_SERVER_API gsa,
                                      ENGINE_HANDLE **handle);

    MEMCACHED_PUBLIC_API
    void destroy_engine(void);
}

/** Binary protocol command used to control this engine. */
const uint8_t PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL = 0xeb;

extern "C" {
    /**
     * Definition of the packet used to control this engine:
     */
    typedef union {
        struct {
            protocol_binary_request_header header;
            struct {
                uint32_t mode; // See EWB_Engine::Mode
                uint32_t value;
            } body;
        } message;
        uint8_t bytes[sizeof(protocol_binary_request_header) +
                      sizeof(uint32_t) +
                      sizeof(uint32_t)];
    } request_ewouldblock_ctl;

    /**
     * Definition of the packet returned by ewouldblock_ctl requests.
     */
    typedef protocol_binary_response_no_extras response_ewouldblock_ctl;
}

/** ewouldblock_engine class */
class EWB_Engine : public ENGINE_HANDLE_V1 {
public:

    // The mode the engine is currently operating in. Determines when it will
    // inject EWOULDBLOCK instead of the real return code.
    enum class Mode {
        NEXT_N, // Make the next_N calls into engine return EWOULDBLOCK.
                // N specified by the {value} field.
        RANDOM, // Randomly return EWOULDBLOCK. Chance to return EWOULDBLOCK is
                // specified as an integer percentage (1,100) in the {value}
                // field.
        FIRST,  // The first call to a given function will return EWOULDBLOCK,
                // with the next (and subsequent) calls to the *same* function
                // operating normally. Calling a different function will reset
                // back to failing again.
                // In other words, return EWOULDBLOCK iif the previous function
                // was not this one.
    };

private:
    enum class Cmd { NONE, GET_INFO, ALLOCATE, REMOVE, GET, STORE, ARITHMETIC,
                     FLUSH, GET_STATS, UNKNOWN_COMMAND };

public:
    EWB_Engine(GET_SERVER_API gsa_);

    ~EWB_Engine();

    // Convert from a handle back to the read object.
    static EWB_Engine* to_engine(ENGINE_HANDLE* handle) {
        return reinterpret_cast<EWB_Engine*> (handle);
    }

    /* Returns true if the next command should return EWOULDBLOCK.
     * @param func Address of the command function (get, store, etc).
     * @param cookie The cookie for the user's request.
     */
    bool should_return_EWOULDBLOCK(Cmd cmd, const void* cookie) {
        bool block = false;
        switch (mode) {
            case Mode::NEXT_N:
                if (value > 0) {
                    --value;
                    block = true;
                }
                break;
            case Mode::RANDOM:
            {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<uint32_t> dis(1, 100);
                if (dis(gen) < value) {
                    block = true;
                }
                break;
            }
            case Mode::FIRST:
            {
                // Block unless the previous command was the same - i.e.
                // all command will EWOULDBLOCK the first time they are called.
                static Cmd prev_cmd = Cmd::NONE;
                block = prev_cmd != cmd;
                prev_cmd = cmd;
            }
        }

        if (block) {
            {
                std::lock_guard<std::mutex> guard(mutex);
                pending_io_ops.push(cookie);
            }
            condvar.notify_one();
        }
        return block;
    }

    /* Implementation of all the engine functions. ***************************/

    static const engine_info* get_info(ENGINE_HANDLE* handle) {
        return &to_engine(handle)->info.eng_info;
    }

    static ENGINE_ERROR_CODE initialize(ENGINE_HANDLE* handle,
                                        const char* config_str) {
        EWB_Engine* ewb = to_engine(handle);

        // Extract the name of the real engine we will be proxying; then
        // create and initialize it.
        std::string config(config_str);
        auto seperator = config.find(";");
        std::string real_engine_name(config.substr(0, seperator));
        std::string real_engine_config;
        if (seperator != std::string::npos) {
            real_engine_config = config.substr(seperator);
        }

        engine_reference* engine_ref = NULL;
        if ((engine_ref = load_engine(real_engine_name.c_str(), NULL)) == NULL) {
            fprintf(stderr,
                    "ERROR: EWB_Engine::initialize(): Failed to load real engine "
                    "'%s'\n",
                    real_engine_name.c_str());
            abort();
        }

        if (!create_engine_instance(engine_ref, ewb->gsa, NULL, &ewb->real_handle)) {
            fprintf(stderr,
                    "ERROR: EWB_Engine::initialize(): Failed create engine instance "
                    "'%s'\n",
                    real_engine_name.c_str());
            abort();
        }

        if (ewb->real_handle->interface != 1) {
            fprintf(stderr, "ERROR: EWB_Engine::initialize(): Only support engine "
                    "with interface v1 - got v%" PRIu64 ".",
                    ewb->real_engine->interface.interface);
            abort();
        }
        ewb->real_engine =
                reinterpret_cast<ENGINE_HANDLE_V1*>(ewb->real_handle);
        ENGINE_ERROR_CODE res = ewb->real_engine->initialize(
                ewb->real_handle, real_engine_config.c_str());

        if (res == ENGINE_SUCCESS) {
            // For engine interface functions which cannot return EWOULDBLOCK, we can
            // simply use the real_engine's functions directly.
            ewb->ENGINE_HANDLE_V1::get_stats_struct = ewb->real_engine->get_stats_struct;
            ewb->ENGINE_HANDLE_V1::item_set_cas = ewb->real_engine->item_set_cas;
            ewb->ENGINE_HANDLE_V1::get_item_info = ewb->real_engine->get_item_info;
            ewb->ENGINE_HANDLE_V1::set_item_info = ewb->real_engine->set_item_info;

            // TODO: Add support for DCP interposing. For now just use the real
            // engines' interface.
            ewb->ENGINE_HANDLE_V1::dcp = ewb->real_engine->dcp;
        }
        return res;
    }

    static void destroy(ENGINE_HANDLE* handle, const bool force) {
        EWB_Engine* ewb = to_engine(handle);
        ewb->real_engine->destroy(ewb->real_handle, force);
        delete ewb;
    }

    static ENGINE_ERROR_CODE allocate(ENGINE_HANDLE* handle, const void* cookie,
                                      item **item, const void* key,
                                      const size_t nkey, const size_t nbytes,
                                      const int flags, const rel_time_t exptime,
                                      uint8_t datatype) {
        EWB_Engine* ewb = to_engine(handle);
        if (ewb->should_return_EWOULDBLOCK(Cmd::ALLOCATE, cookie)) {
            return ENGINE_EWOULDBLOCK;
        } else {
            return ewb->real_engine->allocate(ewb->real_handle, cookie, item,
                                              key, nkey, nbytes, flags, exptime,
                                              datatype);
        }
    }

    static ENGINE_ERROR_CODE remove(ENGINE_HANDLE* handle, const void* cookie,
                                    const void* key, const size_t nkey,
                                    uint64_t* cas, uint16_t vbucket,
                                    mutation_descr_t* mut_info) {
        EWB_Engine* ewb = to_engine(handle);
        if (ewb->should_return_EWOULDBLOCK(Cmd::REMOVE, cookie)) {
            return ENGINE_EWOULDBLOCK;
        } else {
            return ewb->real_engine->remove(ewb->real_handle, cookie, key, nkey,
                                            cas, vbucket, mut_info);
        }
    }

    static void release(ENGINE_HANDLE* handle, const void *cookie, item* item) {
        EWB_Engine* ewb = to_engine(handle);
        return ewb->real_engine->release(ewb->real_handle, cookie, item);
    }

    static ENGINE_ERROR_CODE get(ENGINE_HANDLE* handle, const void* cookie,
                                 item** item, const void* key, const int nkey,
                                 uint16_t vbucket) {
        EWB_Engine* ewb = to_engine(handle);
        if (ewb->should_return_EWOULDBLOCK(Cmd::GET, cookie)) {
            return ENGINE_EWOULDBLOCK;
        } else {
            return ewb->real_engine->get(ewb->real_handle, cookie, item, key,
                                         nkey, vbucket);
        }
    }

    static ENGINE_ERROR_CODE store(ENGINE_HANDLE* handle, const void *cookie,
                                   item* item, uint64_t *cas,
                                   ENGINE_STORE_OPERATION operation,
                                   uint16_t vbucket) {
        EWB_Engine* ewb = to_engine(handle);
        if (ewb->should_return_EWOULDBLOCK(Cmd::STORE,cookie)) {
            return ENGINE_EWOULDBLOCK;
        } else {
            return ewb->real_engine->store(ewb->real_handle, cookie, item, cas,
                                           operation, vbucket);
        }
    }

    static ENGINE_ERROR_CODE arithmetic(ENGINE_HANDLE* handle,
                                        const void* cookie, const void* key,
                                        const int nkey, const bool increment,
                                        const bool create, const uint64_t delta,
                                        const uint64_t initial,
                                        const rel_time_t exptime, item **item,
                                        uint8_t datatype, uint64_t *result,
                                        uint16_t vbucket) {
        EWB_Engine* ewb = to_engine(handle);
        if (ewb->should_return_EWOULDBLOCK(Cmd::ARITHMETIC, cookie)) {
            return ENGINE_EWOULDBLOCK;
        } else {
            return ewb->real_engine->arithmetic(ewb->real_handle, cookie, key,
                                                nkey, increment, create, delta,
                                                initial, exptime, item,
                                                datatype, result, vbucket);
        }
    }

    static ENGINE_ERROR_CODE flush(ENGINE_HANDLE* handle, const void* cookie,
                                   time_t when) {
        // Flush is a little different - it often returns EWOULDBLOCK, and
        // notify_io_complete() just tells the server it can issue it's *next*
        // command (i.e. no need to re-flush). Therefore just pass Flush
        // straight through for now.
        EWB_Engine* ewb = to_engine(handle);
        return ewb->real_engine->flush(ewb->real_handle, cookie, when);
    }

    static ENGINE_ERROR_CODE get_stats(ENGINE_HANDLE* handle,
                                       const void* cookie, const char* stat_key,
                                       int nkey, ADD_STAT add_stat) {
        EWB_Engine* ewb = to_engine(handle);
        if (ewb->should_return_EWOULDBLOCK(Cmd::GET_STATS, cookie)) {
            return ENGINE_EWOULDBLOCK;
        } else {
            return ewb->real_engine->get_stats(ewb->real_handle, cookie, stat_key,
                                               nkey, add_stat);
        }
    }

    static void reset_stats(ENGINE_HANDLE* handle, const void* cookie) {
        EWB_Engine* ewb = to_engine(handle);
        return ewb->real_engine->reset_stats(ewb->real_handle, cookie);
    }

    static void* get_stats_struct(ENGINE_HANDLE* handle, const void* cookie) {
        EWB_Engine* ewb = to_engine(handle);
        return ewb->real_engine->get_stats_struct(ewb->real_handle, cookie);
    }

    static ENGINE_ERROR_CODE unknown_command(ENGINE_HANDLE* handle,
                                             const void* cookie,
                                             protocol_binary_request_header *request,
                                             ADD_RESPONSE response) {
        EWB_Engine* ewb = to_engine(handle);
        if (request->request.opcode == PROTOCOL_BINARY_CMD_EWOULDBLOCK_CTL) {
            request_ewouldblock_ctl* req =
                    reinterpret_cast<request_ewouldblock_ctl*>(request);
            const Mode mode = static_cast<Mode>(ntohl(req->message.body.mode));
            const uint32_t value = ntohl(req->message.body.value);
            switch (mode) {
                case Mode::NEXT_N:
                    ewb->mode = mode;
                    ewb->value = value;
                    fprintf(stderr, "EWB_Engine(): Next %u requests will return EWOULDBLOCK.\n",
                            value);
                    return ENGINE_SUCCESS;
                case Mode::RANDOM:
                    ewb->mode = mode;
                    ewb->value = value;
                    fprintf(stderr, "EWB_Engine(): %u%% of requests will return EWOULDBLOCK.\n",
                            value);
                    return ENGINE_SUCCESS;
                case Mode::FIRST:
                    ewb->mode = mode;
                    fprintf(stderr, "EWB_Engine(): First requests to each function will return EWOULDBLOCK.\n");
                    return ENGINE_SUCCESS;
                default:
                    abort();
            }
        } else {
            if (ewb->should_return_EWOULDBLOCK(Cmd::UNKNOWN_COMMAND, cookie)) {
                return ENGINE_EWOULDBLOCK;
            } else {
                return ewb->real_engine->unknown_command(ewb->real_handle, cookie,
                                                         request, response);
            }
        }
    }

    static void item_set_cas(ENGINE_HANDLE *handle, const void* cookie,
                             item* item, uint64_t cas) {
        // Should never be called as ENGINE_HANDLE_V1::item_set_cas is updated
        // to point to the real_engine once it is initialized. This function
        //only exists so there is a non-NULL value for
        // ENGINE_HANDLE_V1::item_set_cas initially to keep load_engine()
        // happy.
        abort();
    }

    static bool get_item_info(ENGINE_HANDLE *handle, const void *cookie,
                              const item* item, item_info *item_info) {
        // Should never be called - set item_set_cas().
        abort();
    }
    static bool set_item_info(ENGINE_HANDLE *handle, const void *cookie,
                              item* item, const item_info *item_info) {
        // Should never be called - set item_set_cas().
        abort();
    }

    GET_SERVER_API gsa;
    union {
        engine_info eng_info;
        char buffer[sizeof(engine_info) +
                    (sizeof(feature_info) * LAST_REGISTERED_ENGINE_FEATURE)];
    } info;

    // Actual engine we are proxying requests to.
    ENGINE_HANDLE* real_handle;
    ENGINE_HANDLE_V1* real_engine;

private:

    // Current mode of EWOULDBLOCK
    Mode mode;
    // Value associated with the current mode.
    uint32_t value;

    // Handle of the notification thread.
    std::thread notification_thread;
};

void process_pending_queue(SERVER_HANDLE_V1* server) {
    std::unique_lock<std::mutex> lk(mutex);
    while (!stop_notification_thread) {
        condvar.wait(lk);
        if (!pending_io_ops.empty()) {
            const void* cookie = pending_io_ops.front();
            server->cookie->notify_io_complete(cookie, ENGINE_SUCCESS);
            pending_io_ops.pop();
        }
    }
}

EWB_Engine::EWB_Engine(GET_SERVER_API gsa_)
  : gsa(gsa_),
    real_engine(NULL),
    mode(Mode::FIRST),
    value(0),
    notification_thread(process_pending_queue, gsa())
{
    interface.interface = 1;
    ENGINE_HANDLE_V1::get_info = get_info;
    ENGINE_HANDLE_V1::initialize = initialize;
    ENGINE_HANDLE_V1::destroy = destroy;
    ENGINE_HANDLE_V1::allocate = allocate;
    ENGINE_HANDLE_V1::remove = remove;
    ENGINE_HANDLE_V1::release = release;
    ENGINE_HANDLE_V1::get = get;
    ENGINE_HANDLE_V1::store = store;
    ENGINE_HANDLE_V1::arithmetic = arithmetic;
    ENGINE_HANDLE_V1::flush = flush;
    ENGINE_HANDLE_V1::get_stats = get_stats;
    ENGINE_HANDLE_V1::reset_stats = reset_stats;
    ENGINE_HANDLE_V1::aggregate_stats = NULL;
    ENGINE_HANDLE_V1::unknown_command = unknown_command;
    ENGINE_HANDLE_V1::tap_notify = NULL;
    ENGINE_HANDLE_V1::get_tap_iterator = NULL;
    ENGINE_HANDLE_V1::item_set_cas = item_set_cas;
    ENGINE_HANDLE_V1::get_item_info = get_item_info;
    ENGINE_HANDLE_V1::set_item_info = set_item_info;
    ENGINE_HANDLE_V1::get_engine_vb_map = NULL;
    ENGINE_HANDLE_V1::get_stats_struct = NULL;

    std::memset(&info, 0, sizeof(info.buffer));
    info.eng_info.description = "EWOULDBLOCK Engine";
    info.eng_info.features[info.eng_info.num_features++].feature = ENGINE_FEATURE_LRU;
    info.eng_info.features[info.eng_info.num_features++].feature = ENGINE_FEATURE_DATATYPE;

    // Spin up a background thread to perform IO notifications.
}

EWB_Engine::~EWB_Engine() {
    stop_notification_thread = true;
    condvar.notify_all();
    notification_thread.join();

}

ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API gsa,
                                  ENGINE_HANDLE **handle)
{
    if (interface != 1) {
        return ENGINE_ENOTSUP;
    }

    EWB_Engine* engine = new EWB_Engine(gsa);

    *handle = reinterpret_cast<ENGINE_HANDLE*> (engine);
    return ENGINE_SUCCESS;
}

void destroy_engine(void) {
    // nothing todo.
}
