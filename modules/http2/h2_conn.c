/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <ap_mpm.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_log.h>
#include <http_connection.h>
#include <http_protocol.h>
#include <http_request.h>

#include "h2_private.h"
#include "h2_config.h"
#include "h2_ctx.h"
#include "h2_filter.h"
#include "h2_mplx.h"
#include "h2_session.h"
#include "h2_stream.h"
#include "h2_stream_set.h"
#include "h2_h2.h"
#include "h2_task.h"
#include "h2_worker.h"
#include "h2_workers.h"
#include "h2_conn.h"
#include "h2_version.h"

static struct h2_workers *workers;

static h2_mpm_type_t mpm_type = H2_MPM_UNKNOWN;
static module *mpm_module;
static int async_mpm;

static void check_modules(int force) 
{
    static int checked = 0;
    int i;

    if (force || !checked) {
        for (i = 0; ap_loaded_modules[i]; ++i) {
            module *m = ap_loaded_modules[i];
            
            if (!strcmp("event.c", m->name)) {
                mpm_type = H2_MPM_EVENT;
                mpm_module = m;
                break;
            }
            else if (!strcmp("worker.c", m->name)) {
                mpm_type = H2_MPM_WORKER;
                mpm_module = m;
                break;
            }
            else if (!strcmp("prefork.c", m->name)) {
                mpm_type = H2_MPM_PREFORK;
                mpm_module = m;
                break;
            }
        }
        checked = 1;
    }
}

apr_status_t h2_conn_child_init(apr_pool_t *pool, server_rec *s)
{
    const h2_config *config = h2_config_sget(s);
    apr_status_t status = APR_SUCCESS;
    int minw, maxw, max_tx_handles, n;
    int max_threads_per_child = 0;
    int idle_secs = 0;

    check_modules(1);
    
    ap_mpm_query(AP_MPMQ_MAX_THREADS, &max_threads_per_child);
    
    status = ap_mpm_query(AP_MPMQ_IS_ASYNC, &async_mpm);
    if (status != APR_SUCCESS) {
        /* some MPMs do not implemnent this */
        async_mpm = 0;
        status = APR_SUCCESS;
    }

    h2_config_init(pool);
    
    minw = h2_config_geti(config, H2_CONF_MIN_WORKERS);
    maxw = h2_config_geti(config, H2_CONF_MAX_WORKERS);    
    if (minw <= 0) {
        minw = max_threads_per_child;
    }
    if (maxw <= 0) {
        maxw = minw;
    }
    
    /* How many file handles is it safe to use for transfer
     * to the master connection to be streamed out? 
     * Is there a portable APR rlimit on NOFILES? Have not
     * found it. And if, how many of those would we set aside?
     * This leads all into a process wide handle allocation strategy
     * which ultimately would limit the number of accepted connections
     * with the assumption of implicitly reserving n handles for every 
     * connection and requiring modules with excessive needs to allocate
     * from a central pool.
     */
    n = h2_config_geti(config, H2_CONF_SESSION_FILES);
    if (n < 0) {
        max_tx_handles = maxw * 2;
    }
    else {
        max_tx_handles = maxw * n;
    }
    
    ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, s,
                 "h2_workers: min=%d max=%d, mthrpchild=%d, tx_files=%d", 
                 minw, maxw, max_threads_per_child, max_tx_handles);
    workers = h2_workers_create(s, pool, minw, maxw, max_tx_handles);
    
    idle_secs = h2_config_geti(config, H2_CONF_MAX_WORKER_IDLE_SECS);
    h2_workers_set_max_idle_secs(workers, idle_secs);
 
    ap_register_input_filter("H2_IN", h2_filter_core_input,
                             NULL, AP_FTYPE_CONNECTION);
   
    status = h2_mplx_child_init(pool, s);
    
    return status;
}

h2_mpm_type_t h2_conn_mpm_type(void)
{
    check_modules(0);
    return mpm_type;
}

static module *h2_conn_mpm_module(void)
{
    check_modules(0);
    return mpm_module;
}

apr_status_t h2_conn_setup(h2_ctx *ctx, conn_rec *c, request_rec *r)
{
    h2_session *session;
    
    if (!workers) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c, APLOGNO(02911) 
                      "workers not initialized");
        return APR_EGENERAL;
    }
    
    if (r) {
        session = h2_session_rcreate(r, ctx, workers);
    }
    else {
        session = h2_session_create(c, ctx, workers);
    }

    h2_ctx_session_set(ctx, session);
    
    return APR_SUCCESS;
}

apr_status_t h2_conn_run(struct h2_ctx *ctx, conn_rec *c)
{
    apr_status_t status;
    int mpm_state = 0;
    
    do {
        if (c->cs) {
            c->cs->sense = CONN_SENSE_DEFAULT;
        }
        status = h2_session_process(h2_ctx_session_get(ctx), async_mpm);
        
        if (APR_STATUS_IS_EOF(status)) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, c, APLOGNO(03045)
                          "h2_session(%ld): process, closing conn", c->id);
            c->keepalive = AP_CONN_CLOSE;
        }
        else {
            c->keepalive = AP_CONN_KEEPALIVE;
        }
        
        if (ap_mpm_query(AP_MPMQ_MPM_STATE, &mpm_state)) {
            break;
        }
    } while (!async_mpm
             && c->keepalive == AP_CONN_KEEPALIVE 
             && mpm_state != AP_MPMQ_STOPPING);
    
    return DONE;
}

apr_status_t h2_conn_pre_close(struct h2_ctx *ctx, conn_rec *c)
{
    apr_status_t status;
    
    status = h2_session_pre_close(h2_ctx_session_get(ctx), async_mpm);
    if (status == APR_SUCCESS) {
        return DONE; /* This is the same, right? */
    }
    return status;
}


conn_rec *h2_slave_create(conn_rec *master, apr_pool_t *p, 
                          apr_thread_t *thread, apr_socket_t *socket)
{
    conn_rec *c;
    void *cfg;
    
    AP_DEBUG_ASSERT(master);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE3, 0, master,
                  "h2_conn(%ld): created from master", master->id);
    
    /* This is like the slave connection creation from 2.5-DEV. A
     * very efficient way - not sure how compatible this is, since
     * the core hooks are no longer run.
     * But maybe it's is better this way, not sure yet.
     */
    c = (conn_rec *) apr_palloc(p, sizeof(conn_rec));
    if (c == NULL) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, master, 
                      APLOGNO(02913) "h2_task: creating conn");
        return NULL;
    }
    
    memcpy(c, master, sizeof(conn_rec));
           
    /* Replace these */
    c->master                 = master;
    c->pool                   = p;        
    c->current_thread         = thread;
    c->conn_config            = ap_create_conn_config(p);
    c->notes                  = apr_table_make(p, 5);
    c->input_filters          = NULL;
    c->output_filters         = NULL;
    c->bucket_alloc           = apr_bucket_alloc_create(p);
    c->data_in_input_filters  = 0;
    c->data_in_output_filters = 0;
    c->clogging_input_filters = 1;
    c->log                    = NULL;
    c->log_id                 = NULL;
    /* Simulate that we had already a request on this connection. */
    c->keepalives             = 1;
    
    /* TODO: these should be unique to this thread */
    c->sbh                    = master->sbh;
    
    ap_set_module_config(c->conn_config, &core_module, socket);
    cfg = ap_get_module_config(master->conn_config, h2_conn_mpm_module());
    ap_set_module_config(c->conn_config, h2_conn_mpm_module(), cfg);

    return c;
}

