/**
 * Copyright (C) Mellanox Technologies Ltd. 2020-2022.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ec_cuda.h"
#include "utils/ucc_malloc.h"
#include "utils/arch/cpu.h"
#include <cuda_runtime.h>
#include <cuda.h>

static const char *stream_task_modes[] = {
    [UCC_EC_CUDA_TASK_KERNEL]  = "kernel",
    [UCC_EC_CUDA_TASK_MEM_OPS] = "driver",
    [UCC_EC_CUDA_TASK_AUTO]    = "auto",
    [UCC_EC_CUDA_TASK_LAST]    = NULL
};

static const char *task_stream_types[] = {
    [UCC_EC_CUDA_USER_STREAM]      = "user",
    [UCC_EC_CUDA_INTERNAL_STREAM]  = "ucc",
    [UCC_EC_CUDA_TASK_STREAM_LAST] = NULL
};

static ucc_config_field_t ucc_ec_cuda_config_table[] = {
    {"", "", NULL, ucc_offsetof(ucc_ec_cuda_config_t, super),
     UCC_CONFIG_TYPE_TABLE(ucc_ec_config_table)},

    {"STREAM_TASK_MODE", "auto",
     "Mechanism to create stream dependency\n"
     "kernel - use waiting kernel\n"
     "driver - use driver MEM_OPS\n"
     "auto   - runtime automatically chooses best one",
     ucc_offsetof(ucc_ec_cuda_config_t, strm_task_mode),
     UCC_CONFIG_TYPE_ENUM(stream_task_modes)},

    {"TASK_STREAM", "user",
     "Stream for cuda task\n"
     "user - user stream provided in execution engine context\n"
     "ucc  - ucc library internal stream",
     ucc_offsetof(ucc_ec_cuda_config_t, task_strm_type),
     UCC_CONFIG_TYPE_ENUM(task_stream_types)},

    {"STREAM_BLOCKING_WAIT", "1",
     "Stream is blocked until collective operation is done",
     ucc_offsetof(ucc_ec_cuda_config_t, stream_blocking_wait),
     UCC_CONFIG_TYPE_UINT},

    {"EXEC_NUM_WORKERS", "1",
     "Number of thread blocks to use for cuda executor",
     ucc_offsetof(ucc_ec_cuda_config_t, exec_num_workers),
     UCC_CONFIG_TYPE_ULUNITS},

    {"EXEC_NUM_THREADS", "512",
     "Number of thread per block to use for cuda executor",
     ucc_offsetof(ucc_ec_cuda_config_t, exec_num_threads),
     UCC_CONFIG_TYPE_ULUNITS},

    {"EXEC_MAX_TASKS", "128",
     "Maximum number of outstanding tasks per executor",
     ucc_offsetof(ucc_ec_cuda_config_t, exec_max_tasks),
     UCC_CONFIG_TYPE_ULUNITS},

    {NULL}

};

static ucc_status_t ucc_ec_cuda_ee_executor_mpool_chunk_malloc(ucc_mpool_t *mp,
                                                               size_t *size_p,
                                                               void ** chunk_p)
{
    return CUDA_FUNC(cudaHostAlloc((void**)chunk_p, *size_p,
                                   cudaHostAllocMapped));
}

static void ucc_ec_cuda_ee_executor_mpool_chunk_free(ucc_mpool_t *mp,
                                                     void *chunk)
{
    CUDA_FUNC(cudaFreeHost(chunk));
}

static void ucc_ec_cuda_executor_chunk_init(ucc_mpool_t *mp, void *obj,
                                            void *chunk)
{
    ucc_ec_cuda_executor_t *eee       = (ucc_ec_cuda_executor_t*) obj;
    int                     max_tasks = EC_CUDA_CONFIG->exec_max_tasks;

    CUDA_FUNC(cudaHostGetDevicePointer(
                  (void**)(&eee->dev_state), (void *)&eee->state, 0));
    CUDA_FUNC(cudaHostGetDevicePointer(
                  (void**)(&eee->dev_pidx), (void *)&eee->pidx, 0));
    CUDA_FUNC(cudaMalloc((void**)&eee->dev_cidx, sizeof(*eee->dev_cidx)));
    CUDA_FUNC(cudaHostAlloc((void**)&eee->tasks,
                            max_tasks * sizeof(ucc_ee_executor_task_t),
                            cudaHostAllocMapped));
    CUDA_FUNC(cudaHostGetDevicePointer(
                  (void**)(&eee->dev_tasks), (void *)eee->tasks, 0));
    if (ucc_ec_cuda.thread_mode == UCC_THREAD_MULTIPLE) {
        ucc_spinlock_init(&eee->tasks_lock, 0);
    }
}

static void ucc_ec_cuda_executor_chunk_cleanup(ucc_mpool_t *mp, void *obj)
{
    ucc_ec_cuda_executor_t *eee = (ucc_ec_cuda_executor_t*) obj;

    CUDA_FUNC(cudaFree((void*)eee->dev_cidx));
    CUDA_FUNC(cudaFreeHost((void*)eee->tasks));
    if (ucc_ec_cuda.thread_mode == UCC_THREAD_MULTIPLE) {
        ucc_spinlock_destroy(&eee->tasks_lock);
    }
}


static ucc_mpool_ops_t ucc_ec_cuda_ee_executor_mpool_ops = {
    .chunk_alloc   = ucc_ec_cuda_ee_executor_mpool_chunk_malloc,
    .chunk_release = ucc_ec_cuda_ee_executor_mpool_chunk_free,
    .obj_init      = ucc_ec_cuda_executor_chunk_init,
    .obj_cleanup   = ucc_ec_cuda_executor_chunk_cleanup,
};

static ucc_status_t ucc_ec_cuda_stream_req_mpool_chunk_malloc(ucc_mpool_t *mp,
                                                              size_t *size_p,
                                                              void ** chunk_p)
{
    ucc_status_t status;

    status = CUDA_FUNC(cudaHostAlloc((void**)chunk_p, *size_p,
                       cudaHostAllocMapped));
    return status;
}

static void ucc_ec_cuda_stream_req_mpool_chunk_free(ucc_mpool_t *mp,
                                                    void *       chunk)
{
    cudaFreeHost(chunk);
}

static void ucc_ec_cuda_stream_req_init(ucc_mpool_t *mp, void *obj, void *chunk)
{
    ucc_ec_cuda_stream_request_t *req = (ucc_ec_cuda_stream_request_t*) obj;

    CUDA_FUNC(cudaHostGetDevicePointer(
                  (void**)(&req->dev_status), (void *)&req->status, 0));
}

static ucc_mpool_ops_t ucc_ec_cuda_stream_req_mpool_ops = {
    .chunk_alloc   = ucc_ec_cuda_stream_req_mpool_chunk_malloc,
    .chunk_release = ucc_ec_cuda_stream_req_mpool_chunk_free,
    .obj_init      = ucc_ec_cuda_stream_req_init,
    .obj_cleanup   = NULL
};

static void ucc_ec_cuda_event_init(ucc_mpool_t *mp, void *obj, void *chunk)
{
    ucc_ec_cuda_event_t *base = (ucc_ec_cuda_event_t *) obj;

    if (ucc_unlikely(
            cudaSuccess !=
            cudaEventCreateWithFlags(&base->event, cudaEventDisableTiming))) {
        ec_error(&ucc_ec_cuda.super, "cudaEventCreateWithFlags failed");
    }
}

static void ucc_ec_cuda_event_cleanup(ucc_mpool_t *mp, void *obj)
{
    ucc_ec_cuda_event_t *base = (ucc_ec_cuda_event_t *) obj;
    if (ucc_unlikely(cudaSuccess != cudaEventDestroy(base->event))) {
        ec_error(&ucc_ec_cuda.super, "cudaEventDestroy failed");
    }
}

static ucc_mpool_ops_t ucc_ec_cuda_event_mpool_ops = {
    .chunk_alloc   = ucc_mpool_hugetlb_malloc,
    .chunk_release = ucc_mpool_hugetlb_free,
    .obj_init      = ucc_ec_cuda_event_init,
    .obj_cleanup   = ucc_ec_cuda_event_cleanup,
};

ucc_status_t ucc_ec_cuda_post_kernel_stream_task(uint32_t *status,
                                                 int blocking_wait,
                                                 cudaStream_t stream);

static ucc_status_t ucc_ec_cuda_post_driver_stream_task(uint32_t *status,
                                                        int blocking_wait,
                                                        cudaStream_t stream)
{
    CUdeviceptr status_ptr  = (CUdeviceptr)status;

    if (blocking_wait) {
        CUDADRV_FUNC(cuStreamWriteValue32(stream, status_ptr,
                                          UCC_EC_CUDA_TASK_STARTED, 0));
        CUDADRV_FUNC(cuStreamWaitValue32(stream, status_ptr,
                                         UCC_EC_CUDA_TASK_COMPLETED,
                                         CU_STREAM_WAIT_VALUE_EQ));
    }
    CUDADRV_FUNC(cuStreamWriteValue32(stream, status_ptr,
                                      UCC_EC_CUDA_TASK_COMPLETED_ACK, 0));
    return UCC_OK;
}

static ucc_status_t ucc_ec_cuda_init(const ucc_ec_params_t *ec_params)
{
    ucc_ec_cuda_config_t *cfg = EC_CUDA_CONFIG;
    ucc_status_t          status;
    int                   device, num_devices, attr;
    CUdevice              cu_dev;
    CUresult              cu_st;
    cudaError_t           cuda_st;
    const char           *cu_err_st_str;

    ucc_ec_cuda.stream = NULL;
    ucc_strncpy_safe(ucc_ec_cuda.super.config->log_component.name,
                     ucc_ec_cuda.super.super.name,
                     sizeof(ucc_ec_cuda.super.config->log_component.name));
    ucc_ec_cuda.thread_mode = ec_params->thread_mode;
    cuda_st = cudaGetDeviceCount(&num_devices);
    if ((cuda_st != cudaSuccess) || (num_devices == 0)) {
        ec_info(&ucc_ec_cuda.super, "CUDA devices are not found");
        return UCC_ERR_NO_RESOURCE;
    }
    CUDA_CHECK(cudaGetDevice(&device));
    /*create event pool */
    status = ucc_mpool_init(&ucc_ec_cuda.events, 0, sizeof(ucc_ec_cuda_event_t),
                            0, UCC_CACHE_LINE_SIZE, 16, UINT_MAX,
                            &ucc_ec_cuda_event_mpool_ops, UCC_THREAD_MULTIPLE,
                            "CUDA Event Objects");
    if (status != UCC_OK) {
        ec_error(&ucc_ec_cuda.super, "failed to create event pool");
        return status;
    }

    /* create request pool */
    status = ucc_mpool_init(
        &ucc_ec_cuda.strm_reqs, 0, sizeof(ucc_ec_cuda_stream_request_t), 0,
        UCC_CACHE_LINE_SIZE, 16, UINT_MAX, &ucc_ec_cuda_stream_req_mpool_ops,
        UCC_THREAD_MULTIPLE, "CUDA Event Objects");
    if (status != UCC_OK) {
        ec_error(&ucc_ec_cuda.super, "failed to create event pool");
        return status;
    }

     status = ucc_mpool_init(
        &ucc_ec_cuda.executors, 0, sizeof(ucc_ec_cuda_executor_t), 0,
        UCC_CACHE_LINE_SIZE, 16, UINT_MAX, &ucc_ec_cuda_ee_executor_mpool_ops,
        UCC_THREAD_MULTIPLE, "EE executor Objects");
    if (status != UCC_OK) {
        ec_error(&ucc_ec_cuda.super, "failed to create executors pool");
        return status;
    }

    if (cfg->strm_task_mode == UCC_EC_CUDA_TASK_KERNEL) {
        ucc_ec_cuda.strm_task_mode = UCC_EC_CUDA_TASK_KERNEL;
        ucc_ec_cuda.post_strm_task = ucc_ec_cuda_post_kernel_stream_task;
    } else {
        ucc_ec_cuda.strm_task_mode = UCC_EC_CUDA_TASK_MEM_OPS;
        ucc_ec_cuda.post_strm_task = ucc_ec_cuda_post_driver_stream_task;

        cu_st = cuCtxGetDevice(&cu_dev);
        if (cu_st != CUDA_SUCCESS){
            cuGetErrorString(cu_st, &cu_err_st_str);
            ec_debug(&ucc_ec_cuda.super, "cuCtxGetDevice() failed: %s",
                     cu_err_st_str);
            attr = 0;
        } else {
            CUDADRV_FUNC(cuDeviceGetAttribute(&attr,
                        CU_DEVICE_ATTRIBUTE_CAN_USE_STREAM_MEM_OPS,
                        cu_dev));
        }

        if (cfg->strm_task_mode == UCC_EC_CUDA_TASK_AUTO) {
            if (attr == 0) {
                ec_info(&ucc_ec_cuda.super,
                        "CUDA MEM OPS are not supported or disabled");
                ucc_ec_cuda.strm_task_mode = UCC_EC_CUDA_TASK_KERNEL;
                ucc_ec_cuda.post_strm_task = ucc_ec_cuda_post_kernel_stream_task;
            }
        } else if (attr == 0) {
            ec_error(&ucc_ec_cuda.super,
                     "CUDA MEM OPS are not supported or disabled");
            return UCC_ERR_NOT_SUPPORTED;
        }
    }
    ucc_ec_cuda.task_strm_type = cfg->task_strm_type;
    ucc_spinlock_init(&ucc_ec_cuda.init_spinlock, 0);
    return UCC_OK;
}

static ucc_status_t ucc_ec_cuda_get_attr(ucc_ec_attr_t *ec_attr)
{
    if (ec_attr->field_mask & UCC_EC_ATTR_FIELD_THREAD_MODE) {
        ec_attr->thread_mode = ucc_ec_cuda.thread_mode;
    }
    return UCC_OK;
}

ucc_status_t ucc_ec_cuda_task_post(void *ee_stream, void **ee_req)
{
    ucc_ec_cuda_config_t         *cfg = EC_CUDA_CONFIG;
    ucc_ec_cuda_stream_request_t *req;
    ucc_ec_cuda_event_t          *cuda_event;
    ucc_status_t                  status;

    UCC_EC_CUDA_INIT_STREAM();
    req = ucc_mpool_get(&ucc_ec_cuda.strm_reqs);
    ucc_assert(req);
    req->status = UCC_EC_CUDA_TASK_POSTED;
    req->stream = (cudaStream_t)ee_stream;

    if (ucc_ec_cuda.task_strm_type == UCC_EC_CUDA_USER_STREAM) {
        status = ucc_ec_cuda.post_strm_task(req->dev_status,
                                            cfg->stream_blocking_wait,
                                            req->stream);
        if (ucc_unlikely(status != UCC_OK)) {
            goto free_req;
        }
    } else {
        cuda_event = ucc_mpool_get(&ucc_ec_cuda.events);
        ucc_assert(cuda_event);
        CUDA_CHECK(cudaEventRecord(cuda_event->event, req->stream));
        CUDA_CHECK(cudaStreamWaitEvent(ucc_ec_cuda.stream, cuda_event->event, 0));
        status = ucc_ec_cuda.post_strm_task(req->dev_status,
                                            cfg->stream_blocking_wait,
                                            ucc_ec_cuda.stream);
        if (ucc_unlikely(status != UCC_OK)) {
            goto free_event;
        }
        CUDA_CHECK(cudaEventRecord(cuda_event->event, ucc_ec_cuda.stream));
        CUDA_CHECK(cudaStreamWaitEvent(req->stream, cuda_event->event, 0));
        ucc_mpool_put(cuda_event);
    }

    *ee_req = (void *) req;

    ec_info(&ucc_ec_cuda.super, "stream task posted on \"%s\" stream. req:%p",
            task_stream_types[ucc_ec_cuda.task_strm_type], req);

    return UCC_OK;

free_event:
    ucc_mpool_put(cuda_event);
free_req:
    ucc_mpool_put(req);
    return status;
}

ucc_status_t ucc_ec_cuda_task_query(void *ee_req)
{
    ucc_ec_cuda_stream_request_t *req = ee_req;

    /* ee task might be only in POSTED, STARTED or COMPLETED_ACK state
       COMPLETED state is used by ucc_ee_cuda_task_end function to request
       stream unblock*/
    ucc_assert(req->status != UCC_EC_CUDA_TASK_COMPLETED);
    if (req->status == UCC_EC_CUDA_TASK_POSTED) {
        return UCC_INPROGRESS;
    }
    ec_info(&ucc_ec_cuda.super, "stream task started. req:%p", req);
    return UCC_OK;
}

ucc_status_t ucc_ec_cuda_task_end(void *ee_req)
{
    ucc_ec_cuda_stream_request_t *req = ee_req;
    volatile ucc_ec_task_status_t *st = &req->status;

    /* can be safely ended only if it's in STARTED or COMPLETED_ACK state */
    ucc_assert((*st != UCC_EC_CUDA_TASK_POSTED) &&
               (*st != UCC_EC_CUDA_TASK_COMPLETED));
    if (*st == UCC_EC_CUDA_TASK_STARTED) {
        *st = UCC_EC_CUDA_TASK_COMPLETED;
        while(*st != UCC_EC_CUDA_TASK_COMPLETED_ACK) { }
    }
    ucc_mpool_put(req);
    ec_info(&ucc_ec_cuda.super, "stream task done. req:%p", req);
    return UCC_OK;
}

ucc_status_t ucc_ec_cuda_create_event(void **event)
{
    ucc_ec_cuda_event_t *cuda_event;

    cuda_event = ucc_mpool_get(&ucc_ec_cuda.events);
    ucc_assert(cuda_event);
    *event = cuda_event;
    return UCC_OK;
}

ucc_status_t ucc_ec_cuda_destroy_event(void *event)
{
    ucc_ec_cuda_event_t *cuda_event = event;

    ucc_mpool_put(cuda_event);
    return UCC_OK;
}

ucc_status_t ucc_ec_cuda_event_post(void *ee_context, void *event)
{
    cudaStream_t stream = (cudaStream_t )ee_context;
    ucc_ec_cuda_event_t *cuda_event = event;

    CUDA_CHECK(cudaEventRecord(cuda_event->event, stream));
    return UCC_OK;
}

ucc_status_t ucc_ec_cuda_event_test(void *event)
{
    cudaError_t cu_err;
    ucc_ec_cuda_event_t *cuda_event = event;

    cu_err = cudaEventQuery(cuda_event->event);

    if (ucc_unlikely((cu_err != cudaSuccess) &&
                     (cu_err != cudaErrorNotReady))) {
        CUDA_CHECK(cu_err);
    }
    return cuda_error_to_ucc_status(cu_err);
}

ucc_status_t ucc_cuda_executor_init(const ucc_ee_executor_params_t *params,
                                    ucc_ee_executor_t **executor)
{
    ucc_ec_cuda_executor_t *eee = ucc_mpool_get(&ucc_ec_cuda.executors);

    if (ucc_unlikely(!eee)) {
        ec_error(&ucc_ec_cuda.super, "failed to allocate executor");
        return UCC_ERR_NO_MEMORY;
    }
    UCC_EC_CUDA_INIT_STREAM();
    ec_debug(&ucc_ec_cuda.super, "executor init, eee: %p", eee);
    eee->super.ee_type = params->ee_type;
    eee->state         = UCC_EC_CUDA_EXECUTOR_INITIALIZED;

    *executor = &eee->super;
    return UCC_OK;
}

ucc_status_t ucc_cuda_executor_status(const ucc_ee_executor_t *executor)
{
    ucc_ec_cuda_executor_t *eee = ucc_derived_of(executor,
                                                 ucc_ec_cuda_executor_t);

    switch (eee->state) {
    case UCC_EC_CUDA_EXECUTOR_INITIALIZED:
        return UCC_OPERATION_INITIALIZED;
    case UCC_EC_CUDA_EXECUTOR_POSTED:
        return UCC_INPROGRESS;
    case UCC_EC_CUDA_EXECUTOR_STARTED:
        return UCC_OK;
    default:
/* executor has been destroyed */
        return UCC_ERR_NO_RESOURCE;
    }
}

ucc_status_t ucc_ec_cuda_start_executor(ucc_ec_cuda_executor_t *eee);

ucc_status_t ucc_cuda_executor_start(ucc_ee_executor_t *executor,
                                     void *ee_context)
{
    ucc_ec_cuda_executor_t *eee = ucc_derived_of(executor,
                                                 ucc_ec_cuda_executor_t);
    ucc_status_t status;

    ucc_assert(eee->state == UCC_EC_CUDA_EXECUTOR_INITIALIZED);
    ec_debug(&ucc_ec_cuda.super, "executor start, eee: %p", eee);
    eee->super.ee_context = (NULL == ee_context) ? ucc_ec_cuda.stream : ee_context;
    eee->state            = UCC_EC_CUDA_EXECUTOR_POSTED;
    eee->pidx             = 0;

    status = ucc_ec_cuda_start_executor(eee);
    if (status != UCC_OK) {
        ec_error(&ucc_ec_cuda.super, "failed to launch executor kernel");
        return status;
    }

    return UCC_OK;
}

ucc_status_t ucc_cuda_executor_stop(ucc_ee_executor_t *executor)
{
    ucc_ec_cuda_executor_t *eee = ucc_derived_of(executor,
                                                 ucc_ec_cuda_executor_t);
    volatile ucc_ec_cuda_executor_state_t *st = &eee->state;

    ec_debug(&ucc_ec_cuda.super, "executor stop, eee: %p", eee);
    /* can be safely ended only if it's in STARTED or COMPLETED_ACK state */
    ucc_assert((*st != UCC_EC_CUDA_EXECUTOR_POSTED) &&
               (*st != UCC_EC_CUDA_EXECUTOR_SHUTDOWN));
    *st = UCC_EC_CUDA_EXECUTOR_SHUTDOWN;
    eee->pidx = -1;
    while(*st != UCC_EC_CUDA_EXECUTOR_SHUTDOWN_ACK) { }
    eee->super.ee_context = NULL;
    eee->state = UCC_EC_CUDA_EXECUTOR_INITIALIZED;

    return UCC_OK;
}

ucc_status_t ucc_cuda_executor_free(ucc_ee_executor_t *executor)
{
    ucc_ec_cuda_executor_t *eee = ucc_derived_of(executor,
                                                 ucc_ec_cuda_executor_t);

    ec_debug(&ucc_ec_cuda.super, "executor free, eee: %p", eee);
    ucc_assert(eee->state == UCC_EC_CUDA_EXECUTOR_INITIALIZED);
    ucc_mpool_put(eee);

    return UCC_OK;
}

ucc_status_t ucc_cuda_executor_task_test(const ucc_ee_executor_task_t *task)
{
    CUDA_CHECK(cudaGetLastError());
    return task->status;
}

ucc_status_t
ucc_cuda_executor_task_post(ucc_ee_executor_t *executor,
                            const ucc_ee_executor_task_args_t *task_args,
                            ucc_ee_executor_task_t **task)
{
    ucc_ec_cuda_executor_t *eee       = ucc_derived_of(executor,
                                                       ucc_ec_cuda_executor_t);
    int                     max_tasks = EC_CUDA_CONFIG->exec_max_tasks;
    ucc_ee_executor_task_t *ee_task;

    if (ucc_ec_cuda.thread_mode == UCC_THREAD_MULTIPLE) {
        ucc_spin_lock(&eee->tasks_lock);
    }
    ee_task         = &(eee->tasks[eee->pidx % max_tasks]);
    ee_task->eee    = executor;
    ee_task->status = UCC_OPERATION_INITIALIZED;
    memcpy(&ee_task->args, task_args, sizeof(ucc_ee_executor_task_args_t));
    ucc_memory_bus_fence();
    eee->pidx += 1;
    if (ucc_ec_cuda.thread_mode == UCC_THREAD_MULTIPLE) {
        ucc_spin_unlock(&eee->tasks_lock);
    }

    *task = ee_task;
    return UCC_OK;
}

static ucc_status_t ucc_ec_cuda_finalize()
{
    if (ucc_ec_cuda.stream != NULL) {
        CUDA_CHECK(cudaStreamDestroy(ucc_ec_cuda.stream));
        ucc_ec_cuda.stream = NULL;
    }
    ucc_mpool_cleanup(&ucc_ec_cuda.events, 1);
    ucc_mpool_cleanup(&ucc_ec_cuda.strm_reqs, 1);
    ucc_mpool_cleanup(&ucc_ec_cuda.executors, 1);
    return UCC_OK;
}

ucc_ec_cuda_t ucc_ec_cuda = {
    .super.super.name             = "cuda ec",
    .super.ref_cnt                = 0,
    .super.type                   = UCC_EE_CUDA_STREAM,
    .super.init                   = ucc_ec_cuda_init,
    .super.get_attr               = ucc_ec_cuda_get_attr,
    .super.finalize               = ucc_ec_cuda_finalize,
    .super.config_table =
        {
            .name   = "CUDA execution component",
            .prefix = "EC_CUDA_",
            .table  = ucc_ec_cuda_config_table,
            .size   = sizeof(ucc_ec_cuda_config_t),
        },
    .super.ops.task_post          = ucc_ec_cuda_task_post,
    .super.ops.task_query         = ucc_ec_cuda_task_query,
    .super.ops.task_end           = ucc_ec_cuda_task_end,
    .super.ops.create_event       = ucc_ec_cuda_create_event,
    .super.ops.destroy_event      = ucc_ec_cuda_destroy_event,
    .super.ops.event_post         = ucc_ec_cuda_event_post,
    .super.ops.event_test         = ucc_ec_cuda_event_test,
    .super.executor_ops.init      = ucc_cuda_executor_init,
    .super.executor_ops.start     = ucc_cuda_executor_start,
    .super.executor_ops.status    = ucc_cuda_executor_status,
    .super.executor_ops.stop      = ucc_cuda_executor_stop,
    .super.executor_ops.task_post = ucc_cuda_executor_task_post,
    .super.executor_ops.task_test = ucc_cuda_executor_task_test,
};

UCC_CONFIG_REGISTER_TABLE_ENTRY(&ucc_ec_cuda.super.config_table,
                                &ucc_config_global_list);
