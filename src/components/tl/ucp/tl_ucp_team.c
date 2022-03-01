/**
 * Copyright (C) Mellanox Technologies Ltd. 2020-2021.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "tl_ucp.h"
#include "tl_ucp_ep.h"
#include "tl_ucp_coll.h"
#include "tl_ucp_sendrecv.h"
#include "utils/ucc_malloc.h"
#include "coll_score/ucc_coll_score.h"

#ifdef HAVE_DPU_OFFLOAD
#include "dpu_offload_service_daemon.h"
#endif

UCC_CLASS_INIT_FUNC(ucc_tl_ucp_team_t, ucc_base_context_t *tl_context,
                    const ucc_base_team_params_t *params)
{
    ucc_tl_ucp_context_t *ctx =
        ucc_derived_of(tl_context, ucc_tl_ucp_context_t);

    UCC_CLASS_CALL_SUPER_INIT(ucc_tl_team_t, &ctx->super, params);
    /* TODO: init based on ctx settings and on params: need to check
             if all the necessary ranks mappings are provided */
    self->preconnect_task    = NULL;
    self->seq_num            = 0;
    self->status             = UCC_INPROGRESS;
#ifdef HAVE_DPU_OFFLOAD
    fprintf(stderr, "DBG: Initializing team's offloading data...\n");
    self->dpu_offloading_econtext = NULL;
#endif // HAVE_DPU_OFFLOAD

    tl_info(tl_context->lib, "posted tl team: %p", self);
    return UCC_OK;
}

UCC_CLASS_CLEANUP_FUNC(ucc_tl_ucp_team_t)
{
    tl_info(self->super.super.context->lib, "finalizing tl team: %p", self);
#ifdef HAVE_DPU_OFFLOAD
    // Terminate the execution context
    // todo
    self->dpu_offloading_econtext = NULL;
#endif // HAVE_DPU_OFFLOAD
}

UCC_CLASS_DEFINE_DELETE_FUNC(ucc_tl_ucp_team_t, ucc_base_team_t);
UCC_CLASS_DEFINE(ucc_tl_ucp_team_t, ucc_tl_team_t);

ucc_status_t ucc_tl_ucp_team_destroy(ucc_base_team_t *tl_team)
{
    UCC_CLASS_DELETE_FUNC_NAME(ucc_tl_ucp_team_t)(tl_team);
    return UCC_OK;
}

static ucc_status_t ucc_tl_ucp_team_preconnect(ucc_tl_ucp_team_t *team)
{
    ucc_rank_t src, dst, size, rank;
    ucc_status_t status;
    int i;

    size = UCC_TL_TEAM_SIZE(team);
    rank = UCC_TL_TEAM_RANK(team);
    if (!team->preconnect_task) {
        team->preconnect_task = ucc_tl_ucp_get_task(team);
        team->preconnect_task->tag = 0;
    }
    if (UCC_INPROGRESS == ucc_tl_ucp_test(team->preconnect_task)) {
        return UCC_INPROGRESS;
    }
    for (i = team->preconnect_task->send_posted; i < size; i++) {
        src = (rank - i + size) % size;
        dst = (rank + i) % size;
        status = ucc_tl_ucp_send_nb(NULL, 0, UCC_MEMORY_TYPE_UNKNOWN, src, team,
                                    team->preconnect_task);
        if (UCC_OK != status) {
            return status;
        }
        status = ucc_tl_ucp_recv_nb(NULL, 0, UCC_MEMORY_TYPE_UNKNOWN, dst, team,
                                    team->preconnect_task);
        if (UCC_OK != status) {
            return status;
        }
        if (UCC_INPROGRESS == ucc_tl_ucp_test(team->preconnect_task)) {
            return UCC_INPROGRESS;
        }
    }
    tl_debug(UCC_TL_TEAM_LIB(team), "preconnected tl team: %p, num_eps %d",
             team, size);
    ucc_tl_ucp_put_task(team->preconnect_task);
    team->preconnect_task = NULL;
    return UCC_OK;
}

ucc_status_t ucc_tl_ucp_team_create_test(ucc_base_team_t *tl_team)
{
    ucc_tl_ucp_team_t *   team = ucc_derived_of(tl_team, ucc_tl_ucp_team_t);
    ucc_tl_ucp_context_t *ctx  = UCC_TL_UCP_TEAM_CTX(team);
    ucc_status_t          status;

    if (team->status == UCC_OK) {
        return UCC_OK;
    }
    if (UCC_TL_TEAM_SIZE(team) <= ctx->cfg.preconnect) {
        status = ucc_tl_ucp_team_preconnect(team);
        if (UCC_INPROGRESS == status) {
            return UCC_INPROGRESS;
        } else if (UCC_OK != status) {
            goto err_preconnect;
        }
    }

    if (ctx->remote_info) {
        for (int i = 0; i < ctx->n_rinfo_segs; i++) {
            team->va_base[i]     = ctx->remote_info[i].va_base;
            team->base_length[i] = ctx->remote_info[i].len;
        }
    }

#ifdef HAVE_DPU_OFFLOAD
    // DPU offloading: during the initialization of the team, we check if we are
    // connect to the local shadow DPU(s), if not we try to connect to it.
    fprintf(stderr, "DBG: looking up offloading engine...\n");
    offloading_engine_t *offloading_engine = ctx->dpu_offloading_engine;
    fprintf(stderr, "DBG: offloading engine found (%p)...\n", offloading_engine);
    assert(offloading_engine);
    if (offloading_engine->client == NULL)
    {
        fprintf(stderr, "DBG: -> getting library...\n");
        ucc_lib_info_t   *_lib = team->super.super.context->ucc_context->lib;
        execution_context_t *econtext;
        init_params_t offloading_init_params;
        conn_params_t client_conn_params;
        rank_info_t rank_info;
        if (_lib->dpu_offloading.cfg_file != NULL)
        {
            fprintf(stderr, "DBG: -> Platform configuration file exists, loading...\n");
            uint16_t team_id = team->super.super.params.id;
            rank_info.group_id = team_id;
            rank_info.group_rank = UCC_TL_TEAM_RANK(team);

            client_conn_params.addr_str = _lib->dpu_offloading.dpus_config[0].version_1.addr; // fixme: support for more than one DPU
            client_conn_params.port = _lib->dpu_offloading.dpus_config[0].version_1.rank_port;

            offloading_init_params.worker = ctx->ucp_worker;
            offloading_init_params.proc_info = &rank_info;
            offloading_init_params.conn_params = &client_conn_params;
            fprintf(stderr, "DBG: -> DPU offloading: client initialization based on configuration file...\n");
            econtext = client_init(offloading_engine, &offloading_init_params);
        }
        else 
        {
            fprintf(stderr, "DBG: -> DPU offloading: client initialization without initialization parameters...\n");
            econtext = client_init(offloading_engine, NULL);
        }
        team->dpu_offloading_econtext = econtext;
        offloading_engine->client = econtext->client;
        fprintf(stderr, "DBG: -> DPU offloading: client successfully initialized...\n");
    }
#endif // HAVE_DPU_OFFLOAD

    tl_info(tl_team->context->lib, "initialized tl team: %p", team);
    team->status = UCC_OK;
    return UCC_OK;

err_preconnect:
    return status;
}

ucc_status_t ucc_tl_ucp_team_get_scores(ucc_base_team_t   *tl_team,
                                        ucc_coll_score_t **score_p)
{
    ucc_tl_ucp_team_t          *team    = ucc_derived_of(tl_team,
                                                      ucc_tl_ucp_team_t);
    ucc_component_framework_t  *plugins = &ucc_tl_ucp.super.coll_plugins;
    ucc_tl_ucp_context_t       *tl_ctx  = UCC_TL_UCP_TEAM_CTX(team);
    ucc_base_context_t         *ctx     = UCC_TL_TEAM_CTX(team);
    int                         mt_n    = 0;
    ucc_memory_type_t           mem_types[UCC_MEMORY_TYPE_LAST];
    ucc_coll_score_t           *score, *tlcp_score;
    ucc_tl_coll_plugin_iface_t *tlcp;
    ucc_status_t                status;
    unsigned                    i;

    for (i = 0; i < UCC_MEMORY_TYPE_LAST; i++) {
        if (tl_ctx->ucp_memory_types & UCC_BIT(ucc_memtype_to_ucs[i])) {
            tl_debug(tl_team->context->lib,
                     "enable support for memory type %s",
                     ucc_memory_type_names[i]);
            mem_types[mt_n++] = (ucc_memory_type_t)i;
        }
    }

    /* There can be a different logic for different coll_type/mem_type.
       Right now just init everything the same way. */
    status = ucc_coll_score_build_default(tl_team, UCC_TL_UCP_DEFAULT_SCORE,
                              ucc_tl_ucp_coll_init, UCC_TL_UCP_SUPPORTED_COLLS,
                              mem_types, mt_n, &score);
    if (UCC_OK != status) {
        return status;
    }
    for (i = 0; i < UCC_TL_UCP_N_DEFAULT_ALG_SELECT_STR; i++) {
        status = ucc_coll_score_update_from_str(
            ucc_tl_ucp_default_alg_select_str[i], score, UCC_TL_TEAM_SIZE(team),
            ucc_tl_ucp_coll_init, &team->super.super, UCC_TL_UCP_DEFAULT_SCORE,
            ucc_tl_ucp_alg_id_to_init);
        if (UCC_OK != status) {
            tl_error(tl_team->context->lib,
                     "failed to apply default coll select setting: %s",
                     ucc_tl_ucp_default_alg_select_str[i]);
            goto err;
        }
    }
    if (strlen(ctx->score_str) > 0) {
        status = ucc_coll_score_update_from_str(
            ctx->score_str, score, UCC_TL_TEAM_SIZE(team), NULL,
            &team->super.super, UCC_TL_UCP_DEFAULT_SCORE,
            ucc_tl_ucp_alg_id_to_init);

        /* If INVALID_PARAM - User provided incorrect input - try to proceed */
        if ((status < 0) && (status != UCC_ERR_INVALID_PARAM) &&
            (status != UCC_ERR_NOT_SUPPORTED)) {
            goto err;
        }
    }

    for (i = 0; i < plugins->n_components; i++) {
        tlcp = ucc_derived_of(plugins->components[i],
                              ucc_tl_coll_plugin_iface_t);
        status = tlcp->get_scores(tl_team, &tlcp_score);
        if (UCC_OK != status) {
            goto err;
        }
        status = ucc_coll_score_merge_in(&score, tlcp_score);
        if (UCC_OK != status) {
            goto err;
        }
    }
    *score_p = score;
    return UCC_OK;
err:
    ucc_coll_score_free(score);
    return status;
}
