/*
 * H.264 hardware encoding using nvidia nvenc
 * Copyright (c) 2014 Timo Rothenpieler <timo@rothenpieler.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <nvEncodeAPI.h>

#include "libavutil/fifo.h"
#include "libavutil/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/hwcontext.h"
#include "avcodec.h"
#include "internal.h"
#include "thread.h"


#if CONFIG_CUDA
#include <cuda.h>
#include "libavutil/hwcontext_cuda.h"
#else

#if defined(_WIN32)
#define CUDAAPI __stdcall
#else
#define CUDAAPI
#endif

typedef enum cudaError_enum {
    CUDA_SUCCESS = 0
} CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUdeviceptr;
#endif

#if defined(_WIN32)
#define LOAD_FUNC(l, s) GetProcAddress(l, s)
#define DL_CLOSE_FUNC(l) FreeLibrary(l)
#else
#define LOAD_FUNC(l, s) dlsym(l, s)
#define DL_CLOSE_FUNC(l) dlclose(l)
#endif

typedef CUresult(CUDAAPI *PCUINIT)(unsigned int Flags);
typedef CUresult(CUDAAPI *PCUDEVICEGETCOUNT)(int *count);
typedef CUresult(CUDAAPI *PCUDEVICEGET)(CUdevice *device, int ordinal);
typedef CUresult(CUDAAPI *PCUDEVICEGETNAME)(char *name, int len, CUdevice dev);
typedef CUresult(CUDAAPI *PCUDEVICECOMPUTECAPABILITY)(int *major, int *minor, CUdevice dev);
typedef CUresult(CUDAAPI *PCUCTXCREATE)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult(CUDAAPI *PCUCTXPOPCURRENT)(CUcontext *pctx);
typedef CUresult(CUDAAPI *PCUCTXDESTROY)(CUcontext ctx);

typedef NVENCSTATUS (NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST *functionList);

#define MAX_REGISTERED_FRAMES 64
typedef struct NvencSurface
{
    NV_ENC_INPUT_PTR input_surface;
    AVFrame *in_ref;
    NV_ENC_MAP_INPUT_RESOURCE in_map;
    int reg_idx;
    int width;
    int height;

    int lockCount;

    NV_ENC_BUFFER_FORMAT format;

    NV_ENC_OUTPUT_PTR output_surface;
    int size;
} NvencSurface;

typedef struct NvencData
{
    union {
        int64_t timestamp;
        NvencSurface *surface;
    } u;
} NvencData;

typedef struct NvencDynLoadFunctions
{
    PCUINIT cu_init;
    PCUDEVICEGETCOUNT cu_device_get_count;
    PCUDEVICEGET cu_device_get;
    PCUDEVICEGETNAME cu_device_get_name;
    PCUDEVICECOMPUTECAPABILITY cu_device_compute_capability;
    PCUCTXCREATE cu_ctx_create;
    PCUCTXPOPCURRENT cu_ctx_pop_current;
    PCUCTXDESTROY cu_ctx_destroy;

    NV_ENCODE_API_FUNCTION_LIST nvenc_funcs;
    int nvenc_device_count;
    CUdevice nvenc_devices[16];

#if !CONFIG_CUDA
#if defined(_WIN32)
    HMODULE cuda_lib;
#else
    void* cuda_lib;
#endif
#endif
#if defined(_WIN32)
    HMODULE nvenc_lib;
#else
    void* nvenc_lib;
#endif
} NvencDynLoadFunctions;

typedef struct NvencValuePair
{
    const char *str;
    uint32_t num;
} NvencValuePair;

typedef struct NvencContext
{
    AVClass *avclass;

    NvencDynLoadFunctions nvenc_dload_funcs;

    NV_ENC_INITIALIZE_PARAMS init_encode_params;
    NV_ENC_CONFIG encode_config;
    CUcontext cu_context;
    CUcontext cu_context_internal;

    int max_surface_count;
    NvencSurface *surfaces;

    AVFifoBuffer *output_surface_queue;
    AVFifoBuffer *output_surface_ready_queue;
    AVFifoBuffer *timestamp_list;
    int64_t last_dts;

    struct {
        CUdeviceptr ptr;
        NV_ENC_REGISTERED_PTR regptr;
        int mapped;
    } registered_frames[MAX_REGISTERED_FRAMES];
    int nb_registered_frames;

    /* the actual data pixel format, different from
     * AVCodecContext.pix_fmt when using hwaccel frames on input */
    enum AVPixelFormat data_pix_fmt;

    void *nvencoder;

    char *preset;
    char *profile;
    char *level;
    char *tier;
    int cbr;
    int twopass;
    int gpu;
    int buffer_delay;
} NvencContext;

static const NvencValuePair nvenc_h264_level_pairs[] = {
    { "auto", NV_ENC_LEVEL_AUTOSELECT },
    { "1"   , NV_ENC_LEVEL_H264_1     },
    { "1.0" , NV_ENC_LEVEL_H264_1     },
    { "1b"  , NV_ENC_LEVEL_H264_1b    },
    { "1.0b", NV_ENC_LEVEL_H264_1b    },
    { "1.1" , NV_ENC_LEVEL_H264_11    },
    { "1.2" , NV_ENC_LEVEL_H264_12    },
    { "1.3" , NV_ENC_LEVEL_H264_13    },
    { "2"   , NV_ENC_LEVEL_H264_2     },
    { "2.0" , NV_ENC_LEVEL_H264_2     },
    { "2.1" , NV_ENC_LEVEL_H264_21    },
    { "2.2" , NV_ENC_LEVEL_H264_22    },
    { "3"   , NV_ENC_LEVEL_H264_3     },
    { "3.0" , NV_ENC_LEVEL_H264_3     },
    { "3.1" , NV_ENC_LEVEL_H264_31    },
    { "3.2" , NV_ENC_LEVEL_H264_32    },
    { "4"   , NV_ENC_LEVEL_H264_4     },
    { "4.0" , NV_ENC_LEVEL_H264_4     },
    { "4.1" , NV_ENC_LEVEL_H264_41    },
    { "4.2" , NV_ENC_LEVEL_H264_42    },
    { "5"   , NV_ENC_LEVEL_H264_5     },
    { "5.0" , NV_ENC_LEVEL_H264_5     },
    { "5.1" , NV_ENC_LEVEL_H264_51    },
    { NULL }
};

static const NvencValuePair nvenc_hevc_level_pairs[] = {
    { "auto", NV_ENC_LEVEL_AUTOSELECT },
    { "1"   , NV_ENC_LEVEL_HEVC_1     },
    { "1.0" , NV_ENC_LEVEL_HEVC_1     },
    { "2"   , NV_ENC_LEVEL_HEVC_2     },
    { "2.0" , NV_ENC_LEVEL_HEVC_2     },
    { "2.1" , NV_ENC_LEVEL_HEVC_21    },
    { "3"   , NV_ENC_LEVEL_HEVC_3     },
    { "3.0" , NV_ENC_LEVEL_HEVC_3     },
    { "3.1" , NV_ENC_LEVEL_HEVC_31    },
    { "4"   , NV_ENC_LEVEL_HEVC_4     },
    { "4.0" , NV_ENC_LEVEL_HEVC_4     },
    { "4.1" , NV_ENC_LEVEL_HEVC_41    },
    { "5"   , NV_ENC_LEVEL_HEVC_5     },
    { "5.0" , NV_ENC_LEVEL_HEVC_5     },
    { "5.1" , NV_ENC_LEVEL_HEVC_51    },
    { "5.2" , NV_ENC_LEVEL_HEVC_52    },
    { "6"   , NV_ENC_LEVEL_HEVC_6     },
    { "6.0" , NV_ENC_LEVEL_HEVC_6     },
    { "6.1" , NV_ENC_LEVEL_HEVC_61    },
    { "6.2" , NV_ENC_LEVEL_HEVC_62    },
    { NULL }
};

static const struct {
    NVENCSTATUS nverr;
    int         averr;
    const char *desc;
} nvenc_errors[] = {
    { NV_ENC_SUCCESS,                      0,                "success"                  },
    { NV_ENC_ERR_NO_ENCODE_DEVICE,         AVERROR(ENOENT),  "no encode device"         },
    { NV_ENC_ERR_UNSUPPORTED_DEVICE,       AVERROR(ENOSYS),  "unsupported device"       },
    { NV_ENC_ERR_INVALID_ENCODERDEVICE,    AVERROR(EINVAL),  "invalid encoder device"   },
    { NV_ENC_ERR_INVALID_DEVICE,           AVERROR(EINVAL),  "invalid device"           },
    { NV_ENC_ERR_DEVICE_NOT_EXIST,         AVERROR(EIO),     "device does not exist"    },
    { NV_ENC_ERR_INVALID_PTR,              AVERROR(EFAULT),  "invalid ptr"              },
    { NV_ENC_ERR_INVALID_EVENT,            AVERROR(EINVAL),  "invalid event"            },
    { NV_ENC_ERR_INVALID_PARAM,            AVERROR(EINVAL),  "invalid param"            },
    { NV_ENC_ERR_INVALID_CALL,             AVERROR(EINVAL),  "invalid call"             },
    { NV_ENC_ERR_OUT_OF_MEMORY,            AVERROR(ENOMEM),  "out of memory"            },
    { NV_ENC_ERR_ENCODER_NOT_INITIALIZED,  AVERROR(EINVAL),  "encoder not initialized"  },
    { NV_ENC_ERR_UNSUPPORTED_PARAM,        AVERROR(ENOSYS),  "unsupported param"        },
    { NV_ENC_ERR_LOCK_BUSY,                AVERROR(EAGAIN),  "lock busy"                },
    { NV_ENC_ERR_NOT_ENOUGH_BUFFER,        AVERROR(ENOBUFS), "not enough buffer"        },
    { NV_ENC_ERR_INVALID_VERSION,          AVERROR(EINVAL),  "invalid version"          },
    { NV_ENC_ERR_MAP_FAILED,               AVERROR(EIO),     "map failed"               },
    { NV_ENC_ERR_NEED_MORE_INPUT,          AVERROR(EAGAIN),  "need more input"          },
    { NV_ENC_ERR_ENCODER_BUSY,             AVERROR(EAGAIN),  "encoder busy"             },
    { NV_ENC_ERR_EVENT_NOT_REGISTERD,      AVERROR(EBADF),   "event not registered"     },
    { NV_ENC_ERR_GENERIC,                  AVERROR_UNKNOWN,  "generic error"            },
    { NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY,  AVERROR(EINVAL),  "incompatible client key"  },
    { NV_ENC_ERR_UNIMPLEMENTED,            AVERROR(ENOSYS),  "unimplemented"            },
    { NV_ENC_ERR_RESOURCE_REGISTER_FAILED, AVERROR(EIO),     "resource register failed" },
    { NV_ENC_ERR_RESOURCE_NOT_REGISTERED,  AVERROR(EBADF),   "resource not registered"  },
    { NV_ENC_ERR_RESOURCE_NOT_MAPPED,      AVERROR(EBADF),   "resource not mapped"      },
};

static int nvenc_map_error(NVENCSTATUS err, const char **desc)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(nvenc_errors); i++) {
        if (nvenc_errors[i].nverr == err) {
            if (desc)
                *desc = nvenc_errors[i].desc;
            return nvenc_errors[i].averr;
        }
    }
    if (desc)
        *desc = "unknown error";
    return AVERROR_UNKNOWN;
}

static int nvenc_print_error(void *log_ctx, NVENCSTATUS err,
                                     const char *error_string)
{
    const char *desc;
    int ret;
    ret = nvenc_map_error(err, &desc);
    av_log(log_ctx, AV_LOG_ERROR, "%s: %s (%d)\n", error_string, desc, err);
    return ret;
}

static int input_string_to_uint32(AVCodecContext *avctx, const NvencValuePair *pair, const char *input, uint32_t *output)
{
    for (; pair->str; ++pair) {
        if (!strcmp(input, pair->str)) {
            *output = pair->num;
            return 0;
        }
    }

    return AVERROR(EINVAL);
}

static void timestamp_queue_enqueue(AVFifoBuffer* queue, int64_t timestamp)
{
    av_fifo_generic_write(queue, &timestamp, sizeof(timestamp), NULL);
}

static int64_t timestamp_queue_dequeue(AVFifoBuffer* queue)
{
    int64_t timestamp = AV_NOPTS_VALUE;
    if (av_fifo_size(queue) > 0)
        av_fifo_generic_read(queue, &timestamp, sizeof(timestamp), NULL);

    return timestamp;
}

#define CHECK_LOAD_FUNC(t, f, s) \
do { \
    (f) = (t)LOAD_FUNC(dl_fn->cuda_lib, s); \
    if (!(f)) { \
        av_log(avctx, AV_LOG_FATAL, "Failed loading %s from CUDA library\n", s); \
        goto error; \
    } \
} while (0)

static av_cold int nvenc_dyload_cuda(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

#if CONFIG_CUDA
    dl_fn->cu_init                      = cuInit;
    dl_fn->cu_device_get_count          = cuDeviceGetCount;
    dl_fn->cu_device_get                = cuDeviceGet;
    dl_fn->cu_device_get_name           = cuDeviceGetName;
    dl_fn->cu_device_compute_capability = cuDeviceComputeCapability;
    dl_fn->cu_ctx_create                = cuCtxCreate_v2;
    dl_fn->cu_ctx_pop_current           = cuCtxPopCurrent_v2;
    dl_fn->cu_ctx_destroy               = cuCtxDestroy_v2;

    return 1;
#else
    if (dl_fn->cuda_lib)
        return 1;

#if defined(_WIN32)
    dl_fn->cuda_lib = LoadLibrary(TEXT("nvcuda.dll"));
#else
    dl_fn->cuda_lib = dlopen("libcuda.so", RTLD_LAZY);
#endif

    if (!dl_fn->cuda_lib) {
        av_log(avctx, AV_LOG_FATAL, "Failed loading CUDA library\n");
        goto error;
    }

    CHECK_LOAD_FUNC(PCUINIT, dl_fn->cu_init, "cuInit");
    CHECK_LOAD_FUNC(PCUDEVICEGETCOUNT, dl_fn->cu_device_get_count, "cuDeviceGetCount");
    CHECK_LOAD_FUNC(PCUDEVICEGET, dl_fn->cu_device_get, "cuDeviceGet");
    CHECK_LOAD_FUNC(PCUDEVICEGETNAME, dl_fn->cu_device_get_name, "cuDeviceGetName");
    CHECK_LOAD_FUNC(PCUDEVICECOMPUTECAPABILITY, dl_fn->cu_device_compute_capability, "cuDeviceComputeCapability");
    CHECK_LOAD_FUNC(PCUCTXCREATE, dl_fn->cu_ctx_create, "cuCtxCreate_v2");
    CHECK_LOAD_FUNC(PCUCTXPOPCURRENT, dl_fn->cu_ctx_pop_current, "cuCtxPopCurrent_v2");
    CHECK_LOAD_FUNC(PCUCTXDESTROY, dl_fn->cu_ctx_destroy, "cuCtxDestroy_v2");

    return 1;

error:

    if (dl_fn->cuda_lib)
        DL_CLOSE_FUNC(dl_fn->cuda_lib);

    dl_fn->cuda_lib = NULL;

    return 0;
#endif
}

static av_cold int check_cuda_errors(AVCodecContext *avctx, CUresult err, const char *func)
{
    if (err != CUDA_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, ">> %s - failed with error code 0x%x\n", func, err);
        return 0;
    }
    return 1;
}
#define check_cuda_errors(f) if (!check_cuda_errors(avctx, f, #f)) goto error

static av_cold int nvenc_check_cuda(AVCodecContext *avctx)
{
    int device_count = 0;
    CUdevice cu_device = 0;
    char gpu_name[128];
    int smminor = 0, smmajor = 0;
    int i, smver, target_smver;

    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        target_smver = ctx->data_pix_fmt == AV_PIX_FMT_YUV444P ? 0x52 : 0x30;
        break;
    case AV_CODEC_ID_H265:
        target_smver = 0x52;
        break;
    default:
        av_log(avctx, AV_LOG_FATAL, "Unknown codec name\n");
        goto error;
    }

    if (!strncmp(ctx->preset, "lossless", 8))
        target_smver = 0x52;

    if (!nvenc_dyload_cuda(avctx))
        return 0;

    if (dl_fn->nvenc_device_count > 0)
        return 1;

    check_cuda_errors(dl_fn->cu_init(0));

    check_cuda_errors(dl_fn->cu_device_get_count(&device_count));

    if (!device_count) {
        av_log(avctx, AV_LOG_FATAL, "No CUDA capable devices found\n");
        goto error;
    }

    av_log(avctx, AV_LOG_VERBOSE, "%d CUDA capable devices found\n", device_count);

    dl_fn->nvenc_device_count = 0;

    for (i = 0; i < device_count; ++i) {
        check_cuda_errors(dl_fn->cu_device_get(&cu_device, i));
        check_cuda_errors(dl_fn->cu_device_get_name(gpu_name, sizeof(gpu_name), cu_device));
        check_cuda_errors(dl_fn->cu_device_compute_capability(&smmajor, &smminor, cu_device));

        smver = (smmajor << 4) | smminor;

        av_log(avctx, AV_LOG_VERBOSE, "[ GPU #%d - < %s > has Compute SM %d.%d, NVENC %s ]\n", i, gpu_name, smmajor, smminor, (smver >= target_smver) ? "Available" : "Not Available");

        if (smver >= target_smver)
            dl_fn->nvenc_devices[dl_fn->nvenc_device_count++] = cu_device;
    }

    if (!dl_fn->nvenc_device_count) {
        av_log(avctx, AV_LOG_FATAL, "No NVENC capable devices found\n");
        goto error;
    }

    return 1;

error:

    dl_fn->nvenc_device_count = 0;

    return 0;
}

static av_cold int nvenc_dyload_nvenc(AVCodecContext *avctx)
{
    PNVENCODEAPICREATEINSTANCE nvEncodeAPICreateInstance = 0;
    NVENCSTATUS nvstatus;

    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

    if (!nvenc_check_cuda(avctx))
        return 0;

    if (dl_fn->nvenc_lib)
        return 1;

#if defined(_WIN32)
    if (sizeof(void*) == 8) {
        dl_fn->nvenc_lib = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
    } else {
        dl_fn->nvenc_lib = LoadLibrary(TEXT("nvEncodeAPI.dll"));
    }
#else
    dl_fn->nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
#endif

    if (!dl_fn->nvenc_lib) {
        av_log(avctx, AV_LOG_FATAL, "Failed loading the nvenc library\n");
        goto error;
    }

    nvEncodeAPICreateInstance = (PNVENCODEAPICREATEINSTANCE)LOAD_FUNC(dl_fn->nvenc_lib, "NvEncodeAPICreateInstance");

    if (!nvEncodeAPICreateInstance) {
        av_log(avctx, AV_LOG_FATAL, "Failed to load nvenc entrypoint\n");
        goto error;
    }

    dl_fn->nvenc_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    nvstatus = nvEncodeAPICreateInstance(&dl_fn->nvenc_funcs);

    if (nvstatus != NV_ENC_SUCCESS) {
        nvenc_print_error(avctx, nvstatus, "Failed to create nvenc instance");
        goto error;
    }

    av_log(avctx, AV_LOG_VERBOSE, "Nvenc initialized successfully\n");

    return 1;

error:
    if (dl_fn->nvenc_lib)
        DL_CLOSE_FUNC(dl_fn->nvenc_lib);

    dl_fn->nvenc_lib = NULL;

    return 0;
}

static av_cold void nvenc_unload_nvenc(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

    DL_CLOSE_FUNC(dl_fn->nvenc_lib);
    dl_fn->nvenc_lib = NULL;

    dl_fn->nvenc_device_count = 0;

#if !CONFIG_CUDA
    DL_CLOSE_FUNC(dl_fn->cuda_lib);
    dl_fn->cuda_lib = NULL;
#endif

    dl_fn->cu_init = NULL;
    dl_fn->cu_device_get_count = NULL;
    dl_fn->cu_device_get = NULL;
    dl_fn->cu_device_get_name = NULL;
    dl_fn->cu_device_compute_capability = NULL;
    dl_fn->cu_ctx_create = NULL;
    dl_fn->cu_ctx_pop_current = NULL;
    dl_fn->cu_ctx_destroy = NULL;

    av_log(avctx, AV_LOG_VERBOSE, "Nvenc unloaded\n");
}

static av_cold int nvenc_setup_device(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;

    CUresult cu_res;
    CUcontext cu_context_curr;

    ctx->data_pix_fmt = avctx->pix_fmt;

#if CONFIG_CUDA
    if (avctx->pix_fmt == AV_PIX_FMT_CUDA) {
        AVHWFramesContext *frames_ctx;
        AVCUDADeviceContext *device_hwctx;

        if (!avctx->hw_frames_ctx) {
            av_log(avctx, AV_LOG_ERROR, "hw_frames_ctx must be set when using GPU frames as input\n");
            return AVERROR(EINVAL);
        }

        frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        device_hwctx = frames_ctx->device_ctx->hwctx;
        ctx->cu_context = device_hwctx->cuda_ctx;
        ctx->data_pix_fmt = frames_ctx->sw_format;
        return 0;
    }
#endif

    if (ctx->gpu >= dl_fn->nvenc_device_count) {
        av_log(avctx, AV_LOG_FATAL, "Requested GPU %d, but only %d GPUs are available!\n", ctx->gpu, dl_fn->nvenc_device_count);
        return AVERROR(EINVAL);
    }

    ctx->cu_context = NULL;
    cu_res = dl_fn->cu_ctx_create(&ctx->cu_context_internal, 4, dl_fn->nvenc_devices[ctx->gpu]); // CU_CTX_SCHED_BLOCKING_SYNC=4, avoid CPU spins

    if (cu_res != CUDA_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, "Failed creating CUDA context for NVENC: 0x%x\n", (int)cu_res);
        return AVERROR_EXTERNAL;
    }

    cu_res = dl_fn->cu_ctx_pop_current(&cu_context_curr);

    if (cu_res != CUDA_SUCCESS) {
        av_log(avctx, AV_LOG_FATAL, "Failed popping CUDA context: 0x%x\n", (int)cu_res);
        return AVERROR_EXTERNAL;
    }

    ctx->cu_context = ctx->cu_context_internal;

    return 0;
}

static av_cold int nvenc_open_session(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS encode_session_params = { 0 };
    NVENCSTATUS nv_status;

    encode_session_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    encode_session_params.apiVersion = NVENCAPI_VERSION;
    encode_session_params.device = ctx->cu_context;
    encode_session_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;

    nv_status = p_nvenc->nvEncOpenEncodeSessionEx(&encode_session_params, &ctx->nvencoder);
    if (nv_status != NV_ENC_SUCCESS) {
        ctx->nvencoder = NULL;
        return nvenc_print_error(avctx, nv_status, "OpenEncodeSessionEx failed");
    }

    return 0;
}

static av_cold void set_constqp(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;

    ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    ctx->encode_config.rcParams.constQP.qpInterB = avctx->global_quality;
    ctx->encode_config.rcParams.constQP.qpInterP = avctx->global_quality;
    ctx->encode_config.rcParams.constQP.qpIntra = avctx->global_quality;
}

static av_cold void set_vbr(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;

    ctx->encode_config.rcParams.enableMinQP = 1;
    ctx->encode_config.rcParams.enableMaxQP = 1;

    ctx->encode_config.rcParams.minQP.qpInterB = avctx->qmin;
    ctx->encode_config.rcParams.minQP.qpInterP = avctx->qmin;
    ctx->encode_config.rcParams.minQP.qpIntra = avctx->qmin;

    ctx->encode_config.rcParams.maxQP.qpInterB = avctx->qmax;
    ctx->encode_config.rcParams.maxQP.qpInterP = avctx->qmax;
    ctx->encode_config.rcParams.maxQP.qpIntra = avctx->qmax;
}

static av_cold void set_lossless(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;

    ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    ctx->encode_config.rcParams.constQP.qpInterB = 0;
    ctx->encode_config.rcParams.constQP.qpInterP = 0;
    ctx->encode_config.rcParams.constQP.qpIntra = 0;
}

static av_cold void nvenc_setup_rate_control(AVCodecContext *avctx, int lossless)
{
    NvencContext *ctx = avctx->priv_data;

    int qp_inter_p;

    if (avctx->bit_rate > 0) {
        ctx->encode_config.rcParams.averageBitRate = avctx->bit_rate;
    } else if (ctx->encode_config.rcParams.averageBitRate > 0) {
        ctx->encode_config.rcParams.maxBitRate = ctx->encode_config.rcParams.averageBitRate;
    }

    if (avctx->rc_max_rate > 0)
        ctx->encode_config.rcParams.maxBitRate = avctx->rc_max_rate;

    if (lossless) {
        if (avctx->codec->id == AV_CODEC_ID_H264)
            ctx->encode_config.encodeCodecConfig.h264Config.qpPrimeYZeroTransformBypassFlag = 1;

        set_lossless(avctx);

        avctx->qmin = -1;
        avctx->qmax = -1;
    } else if (ctx->cbr) {
        if (!ctx->twopass) {
            ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        } else {
            ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_2_PASS_QUALITY;

            if (avctx->codec->id == AV_CODEC_ID_H264) {
                ctx->encode_config.encodeCodecConfig.h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
                ctx->encode_config.encodeCodecConfig.h264Config.fmoMode = NV_ENC_H264_FMO_DISABLE;
            }
        }

        if (avctx->codec->id == AV_CODEC_ID_H264) {
            ctx->encode_config.encodeCodecConfig.h264Config.outputBufferingPeriodSEI = 1;
            ctx->encode_config.encodeCodecConfig.h264Config.outputPictureTimingSEI = 1;
        } else if (avctx->codec->id == AV_CODEC_ID_H265) {
            ctx->encode_config.encodeCodecConfig.hevcConfig.outputBufferingPeriodSEI = 1;
            ctx->encode_config.encodeCodecConfig.hevcConfig.outputPictureTimingSEI = 1;
        }
    } else if (avctx->global_quality > 0) {
        set_constqp(avctx);

        avctx->qmin = -1;
        avctx->qmax = -1;
    } else {
        if (avctx->qmin >= 0 && avctx->qmax >= 0) {
            set_vbr(avctx);

            qp_inter_p = (avctx->qmax + 3 * avctx->qmin) / 4; // biased towards Qmin

            if (ctx->twopass) {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_2_PASS_VBR;
                if (avctx->codec->id == AV_CODEC_ID_H264) {
                    ctx->encode_config.encodeCodecConfig.h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
                    ctx->encode_config.encodeCodecConfig.h264Config.fmoMode = NV_ENC_H264_FMO_DISABLE;
                }
            } else {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR_MINQP;
            }
        } else {
            qp_inter_p = 26; // default to 26

            if (ctx->twopass) {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_2_PASS_VBR;
            } else {
                ctx->encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            }
        }

        ctx->encode_config.rcParams.enableInitialRCQP = 1;
        ctx->encode_config.rcParams.initialRCQP.qpInterP  = qp_inter_p;

        if (avctx->i_quant_factor != 0.0 && avctx->b_quant_factor != 0.0) {
            ctx->encode_config.rcParams.initialRCQP.qpIntra = av_clip(
                qp_inter_p * fabs(avctx->i_quant_factor) + avctx->i_quant_offset, 0, 51);
            ctx->encode_config.rcParams.initialRCQP.qpInterB = av_clip(
                qp_inter_p * fabs(avctx->b_quant_factor) + avctx->b_quant_offset, 0, 51);
        } else {
            ctx->encode_config.rcParams.initialRCQP.qpIntra = qp_inter_p;
            ctx->encode_config.rcParams.initialRCQP.qpInterB = qp_inter_p;
        }
    }

    if (avctx->rc_buffer_size > 0) {
        ctx->encode_config.rcParams.vbvBufferSize = avctx->rc_buffer_size;
    } else if (ctx->encode_config.rcParams.averageBitRate > 0) {
        ctx->encode_config.rcParams.vbvBufferSize = 2 * ctx->encode_config.rcParams.averageBitRate;
    }
}

static av_cold int nvenc_setup_h264_config(AVCodecContext *avctx, int lossless)
{
    NvencContext *ctx = avctx->priv_data;
    int res;

    ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix = avctx->colorspace;
    ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries = avctx->color_primaries;
    ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics = avctx->color_trc;
    ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag = (avctx->color_range == AVCOL_RANGE_JPEG
        || ctx->data_pix_fmt == AV_PIX_FMT_YUVJ420P || ctx->data_pix_fmt == AV_PIX_FMT_YUVJ422P || ctx->data_pix_fmt == AV_PIX_FMT_YUVJ444P);

    ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag =
        (avctx->colorspace != 2 || avctx->color_primaries != 2 || avctx->color_trc != 2);

    ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.videoSignalTypePresentFlag =
        (ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag
        || ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.videoFormat != 5
        || ctx->encode_config.encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag != 0);

    ctx->encode_config.encodeCodecConfig.h264Config.sliceMode = 3;
    ctx->encode_config.encodeCodecConfig.h264Config.sliceModeData = 1;

    ctx->encode_config.encodeCodecConfig.h264Config.disableSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : 0;
    ctx->encode_config.encodeCodecConfig.h264Config.repeatSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 0 : 1;

    ctx->encode_config.encodeCodecConfig.h264Config.outputAUD = 1;

    if (!ctx->profile && !lossless) {
        switch (avctx->profile) {
        case FF_PROFILE_H264_HIGH_444_PREDICTIVE:
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
            break;
        case FF_PROFILE_H264_BASELINE:
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
            break;
        case FF_PROFILE_H264_MAIN:
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
            break;
        case FF_PROFILE_H264_HIGH:
        case FF_PROFILE_UNKNOWN:
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
            break;
        default:
            av_log(avctx, AV_LOG_WARNING, "Unsupported profile requested, falling back to high\n");
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
            break;
        }
    } else if (!lossless) {
        if (!strcmp(ctx->profile, "high")) {
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
            avctx->profile = FF_PROFILE_H264_HIGH;
        } else if (!strcmp(ctx->profile, "main")) {
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
            avctx->profile = FF_PROFILE_H264_MAIN;
        } else if (!strcmp(ctx->profile, "baseline")) {
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
            avctx->profile = FF_PROFILE_H264_BASELINE;
        } else if (!strcmp(ctx->profile, "high444p")) {
            ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
            avctx->profile = FF_PROFILE_H264_HIGH_444_PREDICTIVE;
        } else {
            av_log(avctx, AV_LOG_FATAL, "Profile \"%s\" is unknown! Supported profiles: high, main, baseline\n", ctx->profile);
            return AVERROR(EINVAL);
        }
    }

    // force setting profile as high444p if input is AV_PIX_FMT_YUV444P
    if (ctx->data_pix_fmt == AV_PIX_FMT_YUV444P) {
        ctx->encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
        avctx->profile = FF_PROFILE_H264_HIGH_444_PREDICTIVE;
    }

    ctx->encode_config.encodeCodecConfig.h264Config.chromaFormatIDC = avctx->profile == FF_PROFILE_H264_HIGH_444_PREDICTIVE ? 3 : 1;

    if (ctx->level) {
        res = input_string_to_uint32(avctx, nvenc_h264_level_pairs, ctx->level, &ctx->encode_config.encodeCodecConfig.h264Config.level);

        if (res) {
            av_log(avctx, AV_LOG_FATAL, "Level \"%s\" is unknown! Supported levels: auto, 1, 1b, 1.1, 1.2, 1.3, 2, 2.1, 2.2, 3, 3.1, 3.2, 4, 4.1, 4.2, 5, 5.1\n", ctx->level);
            return res;
        }
    } else {
        ctx->encode_config.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
    }

    return 0;
}

static av_cold int nvenc_setup_hevc_config(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    int res;

    ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix = avctx->colorspace;
    ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries = avctx->color_primaries;
    ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics = avctx->color_trc;
    ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag = (avctx->color_range == AVCOL_RANGE_JPEG
        || ctx->data_pix_fmt == AV_PIX_FMT_YUVJ420P || ctx->data_pix_fmt == AV_PIX_FMT_YUVJ422P || ctx->data_pix_fmt == AV_PIX_FMT_YUVJ444P);

    ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag =
        (avctx->colorspace != 2 || avctx->color_primaries != 2 || avctx->color_trc != 2);

    ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag =
        (ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag
        || ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFormat != 5
        || ctx->encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag != 0);

    ctx->encode_config.encodeCodecConfig.hevcConfig.sliceMode = 3;
    ctx->encode_config.encodeCodecConfig.hevcConfig.sliceModeData = 1;

    ctx->encode_config.encodeCodecConfig.hevcConfig.disableSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : 0;
    ctx->encode_config.encodeCodecConfig.hevcConfig.repeatSPSPPS = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 0 : 1;

    ctx->encode_config.encodeCodecConfig.hevcConfig.outputAUD = 1;

    /* No other profile is supported in the current SDK version 5 */
    ctx->encode_config.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    avctx->profile = FF_PROFILE_HEVC_MAIN;

    if (ctx->level) {
        res = input_string_to_uint32(avctx, nvenc_hevc_level_pairs, ctx->level, &ctx->encode_config.encodeCodecConfig.hevcConfig.level);

        if (res) {
            av_log(avctx, AV_LOG_FATAL, "Level \"%s\" is unknown! Supported levels: auto, 1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, 6.2\n", ctx->level);
            return res;
        }
    } else {
        ctx->encode_config.encodeCodecConfig.hevcConfig.level = NV_ENC_LEVEL_AUTOSELECT;
    }

    if (ctx->tier) {
        if (!strcmp(ctx->tier, "main")) {
            ctx->encode_config.encodeCodecConfig.hevcConfig.tier = NV_ENC_TIER_HEVC_MAIN;
        } else if (!strcmp(ctx->tier, "high")) {
            ctx->encode_config.encodeCodecConfig.hevcConfig.tier = NV_ENC_TIER_HEVC_HIGH;
        } else {
            av_log(avctx, AV_LOG_FATAL, "Tier \"%s\" is unknown! Supported tiers: main, high\n", ctx->tier);
            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static av_cold int nvenc_setup_codec_config(AVCodecContext *avctx, int lossless)
{
    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        return nvenc_setup_h264_config(avctx, lossless);
    case AV_CODEC_ID_H265:
        return nvenc_setup_hevc_config(avctx);
    /* Earlier switch/case will return if unknown codec is passed. */
    }

    return 0;
}

static av_cold int nvenc_setup_encoder(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    NV_ENC_PRESET_CONFIG preset_config = { 0 };
    GUID encoder_preset = NV_ENC_PRESET_HQ_GUID;
    GUID codec;
    NVENCSTATUS nv_status = NV_ENC_SUCCESS;
    AVCPBProperties *cpb_props;
    int num_mbs;
    int isLL = 0;
    int lossless = 0;
    int res = 0;
    int dw, dh;

    ctx->last_dts = AV_NOPTS_VALUE;

    ctx->encode_config.version = NV_ENC_CONFIG_VER;
    ctx->init_encode_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

    if (ctx->preset) {
        if (!strcmp(ctx->preset, "slow")) {
            encoder_preset = NV_ENC_PRESET_HQ_GUID;
            ctx->twopass = 1;
        } else if (!strcmp(ctx->preset, "medium")) {
            encoder_preset = NV_ENC_PRESET_HQ_GUID;
            ctx->twopass = 0;
        } else if (!strcmp(ctx->preset, "fast")) {
            encoder_preset = NV_ENC_PRESET_HP_GUID;
            ctx->twopass = 0;
        } else if (!strcmp(ctx->preset, "hq")) {
            encoder_preset = NV_ENC_PRESET_HQ_GUID;
        } else if (!strcmp(ctx->preset, "hp")) {
            encoder_preset = NV_ENC_PRESET_HP_GUID;
        } else if (!strcmp(ctx->preset, "bd")) {
            encoder_preset = NV_ENC_PRESET_BD_GUID;
        } else if (!strcmp(ctx->preset, "ll")) {
            encoder_preset = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;
            isLL = 1;
        } else if (!strcmp(ctx->preset, "llhp")) {
            encoder_preset = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
            isLL = 1;
        } else if (!strcmp(ctx->preset, "llhq")) {
            encoder_preset = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
            isLL = 1;
        } else if (!strcmp(ctx->preset, "lossless")) {
            encoder_preset = NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
            lossless = 1;
        } else if (!strcmp(ctx->preset, "losslesshp")) {
            encoder_preset = NV_ENC_PRESET_LOSSLESS_HP_GUID;
            lossless = 1;
        } else if (!strcmp(ctx->preset, "default")) {
            encoder_preset = NV_ENC_PRESET_DEFAULT_GUID;
        } else {
            av_log(avctx, AV_LOG_FATAL, "Preset \"%s\" is unknown! Supported presets: slow, medium, fast, hp, hq, bd, ll, llhp, llhq, lossless, losslesshp, default\n", ctx->preset);
            return AVERROR(EINVAL);
        }
    }

    if (ctx->twopass < 0) {
        ctx->twopass = isLL;
    }

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        codec = NV_ENC_CODEC_H264_GUID;
        break;
    case AV_CODEC_ID_H265:
        codec = NV_ENC_CODEC_HEVC_GUID;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown codec name\n");
        return AVERROR(EINVAL);
    }

    nv_status = p_nvenc->nvEncGetEncodePresetConfig(ctx->nvencoder, codec, encoder_preset, &preset_config);
    if (nv_status != NV_ENC_SUCCESS) {
        return nvenc_print_error(avctx, nv_status, "GetEncodePresetConfig failed");
    }

    ctx->init_encode_params.encodeGUID = codec;
    ctx->init_encode_params.encodeHeight = avctx->height;
    ctx->init_encode_params.encodeWidth = avctx->width;

    if (avctx->sample_aspect_ratio.num && avctx->sample_aspect_ratio.den &&
        (avctx->sample_aspect_ratio.num != 1 || avctx->sample_aspect_ratio.num != 1)) {
        av_reduce(&dw, &dh,
                  avctx->width * avctx->sample_aspect_ratio.num,
                  avctx->height * avctx->sample_aspect_ratio.den,
                  1024 * 1024);
        ctx->init_encode_params.darHeight = dh;
        ctx->init_encode_params.darWidth = dw;
    } else {
        ctx->init_encode_params.darHeight = avctx->height;
        ctx->init_encode_params.darWidth = avctx->width;
    }

    // De-compensate for hardware, dubiously, trying to compensate for
    // playback at 704 pixel width.
    if (avctx->width == 720 &&
        (avctx->height == 480 || avctx->height == 576)) {
        av_reduce(&dw, &dh,
                  ctx->init_encode_params.darWidth * 44,
                  ctx->init_encode_params.darHeight * 45,
                  1024 * 1024);
        ctx->init_encode_params.darHeight = dh;
        ctx->init_encode_params.darWidth = dw;
    }

    ctx->init_encode_params.frameRateNum = avctx->time_base.den;
    ctx->init_encode_params.frameRateDen = avctx->time_base.num * avctx->ticks_per_frame;

    num_mbs = ((avctx->width + 15) >> 4) * ((avctx->height + 15) >> 4);
    ctx->max_surface_count = (num_mbs >= 8160) ? 32 : 48;

    if (ctx->buffer_delay >= ctx->max_surface_count)
        ctx->buffer_delay = ctx->max_surface_count - 1;

    ctx->init_encode_params.enableEncodeAsync = 0;
    ctx->init_encode_params.enablePTD = 1;

    ctx->init_encode_params.presetGUID = encoder_preset;

    ctx->init_encode_params.encodeConfig = &ctx->encode_config;
    memcpy(&ctx->encode_config, &preset_config.presetCfg, sizeof(ctx->encode_config));
    ctx->encode_config.version = NV_ENC_CONFIG_VER;

    if (avctx->refs >= 0) {
        /* 0 means "let the hardware decide" */
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            ctx->encode_config.encodeCodecConfig.h264Config.maxNumRefFrames = avctx->refs;
            break;
        case AV_CODEC_ID_H265:
            ctx->encode_config.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = avctx->refs;
            break;
        /* Earlier switch/case will return if unknown codec is passed. */
        }
    }

    if (avctx->gop_size > 0) {
        if (avctx->max_b_frames >= 0) {
            /* 0 is intra-only, 1 is I/P only, 2 is one B Frame, 3 two B frames, and so on. */
            ctx->encode_config.frameIntervalP = avctx->max_b_frames + 1;
        }

        ctx->encode_config.gopLength = avctx->gop_size;
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            ctx->encode_config.encodeCodecConfig.h264Config.idrPeriod = avctx->gop_size;
            break;
        case AV_CODEC_ID_H265:
            ctx->encode_config.encodeCodecConfig.hevcConfig.idrPeriod = avctx->gop_size;
            break;
        /* Earlier switch/case will return if unknown codec is passed. */
        }
    } else if (avctx->gop_size == 0) {
        ctx->encode_config.frameIntervalP = 0;
        ctx->encode_config.gopLength = 1;
        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            ctx->encode_config.encodeCodecConfig.h264Config.idrPeriod = 1;
            break;
        case AV_CODEC_ID_H265:
            ctx->encode_config.encodeCodecConfig.hevcConfig.idrPeriod = 1;
            break;
        /* Earlier switch/case will return if unknown codec is passed. */
        }
    }

    /* when there're b frames, set dts offset */
    if (ctx->encode_config.frameIntervalP >= 2)
        ctx->last_dts = -2;

    nvenc_setup_rate_control(avctx, lossless);

    if (avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) {
        ctx->encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
    } else {
        ctx->encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    }

    res = nvenc_setup_codec_config(avctx, lossless);
    if (res)
        return res;

    nv_status = p_nvenc->nvEncInitializeEncoder(ctx->nvencoder, &ctx->init_encode_params);
    if (nv_status != NV_ENC_SUCCESS) {
        return nvenc_print_error(avctx, nv_status, "InitializeEncoder failed");
    }

    if (ctx->encode_config.frameIntervalP > 1)
        avctx->has_b_frames = 2;

    if (ctx->encode_config.rcParams.averageBitRate > 0)
        avctx->bit_rate = ctx->encode_config.rcParams.averageBitRate;

    cpb_props = ff_add_cpb_side_data(avctx);
    if (!cpb_props)
        return AVERROR(ENOMEM);
    cpb_props->max_bitrate = ctx->encode_config.rcParams.maxBitRate;
    cpb_props->avg_bitrate = avctx->bit_rate;
    cpb_props->buffer_size = ctx->encode_config.rcParams.vbvBufferSize;

    return 0;
}

static av_cold int nvenc_alloc_surface(AVCodecContext *avctx, int idx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    NVENCSTATUS nv_status;
    NV_ENC_CREATE_BITSTREAM_BUFFER allocOut = { 0 };
    allocOut.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    switch (ctx->data_pix_fmt) {
    case AV_PIX_FMT_YUV420P:
        ctx->surfaces[idx].format = NV_ENC_BUFFER_FORMAT_YV12_PL;
        break;

    case AV_PIX_FMT_NV12:
        ctx->surfaces[idx].format = NV_ENC_BUFFER_FORMAT_NV12_PL;
        break;

    case AV_PIX_FMT_YUV444P:
        ctx->surfaces[idx].format = NV_ENC_BUFFER_FORMAT_YUV444_PL;
        break;

    default:
        av_log(avctx, AV_LOG_FATAL, "Invalid input pixel format\n");
        return AVERROR(EINVAL);
    }

    if (avctx->pix_fmt == AV_PIX_FMT_CUDA) {
        ctx->surfaces[idx].in_ref = av_frame_alloc();
        if (!ctx->surfaces[idx].in_ref)
            return AVERROR(ENOMEM);
    } else {
        NV_ENC_CREATE_INPUT_BUFFER allocSurf = { 0 };
        allocSurf.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        allocSurf.width = (avctx->width + 31) & ~31;
        allocSurf.height = (avctx->height + 31) & ~31;
        allocSurf.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
        allocSurf.bufferFmt = ctx->surfaces[idx].format;

        nv_status = p_nvenc->nvEncCreateInputBuffer(ctx->nvencoder, &allocSurf);
        if (nv_status != NV_ENC_SUCCESS) {
            return nvenc_print_error(avctx, nv_status, "CreateInputBuffer failed");
        }

        ctx->surfaces[idx].input_surface = allocSurf.inputBuffer;
        ctx->surfaces[idx].width = allocSurf.width;
        ctx->surfaces[idx].height = allocSurf.height;
    }

    ctx->surfaces[idx].lockCount = 0;

    /* 1MB is large enough to hold most output frames. NVENC increases this automaticaly if it's not enough. */
    allocOut.size = 1024 * 1024;

    allocOut.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

    nv_status = p_nvenc->nvEncCreateBitstreamBuffer(ctx->nvencoder, &allocOut);
    if (nv_status != NV_ENC_SUCCESS) {
        int err = nvenc_print_error(avctx, nv_status, "CreateBitstreamBuffer failed");
        if (avctx->pix_fmt != AV_PIX_FMT_CUDA)
            p_nvenc->nvEncDestroyInputBuffer(ctx->nvencoder, ctx->surfaces[idx].input_surface);
        av_frame_free(&ctx->surfaces[idx].in_ref);
        return err;
    }

    ctx->surfaces[idx].output_surface = allocOut.bitstreamBuffer;
    ctx->surfaces[idx].size = allocOut.size;

    return 0;
}

static av_cold int nvenc_setup_surfaces(AVCodecContext *avctx, int* surfaceCount)
{
    int res;
    NvencContext *ctx = avctx->priv_data;

    ctx->surfaces = av_malloc(ctx->max_surface_count * sizeof(*ctx->surfaces));

    if (!ctx->surfaces) {
        return AVERROR(ENOMEM);
    }

    ctx->timestamp_list = av_fifo_alloc(ctx->max_surface_count * sizeof(int64_t));
    if (!ctx->timestamp_list)
        return AVERROR(ENOMEM);
    ctx->output_surface_queue = av_fifo_alloc(ctx->max_surface_count * sizeof(NvencSurface*));
    if (!ctx->output_surface_queue)
        return AVERROR(ENOMEM);
    ctx->output_surface_ready_queue = av_fifo_alloc(ctx->max_surface_count * sizeof(NvencSurface*));
    if (!ctx->output_surface_ready_queue)
        return AVERROR(ENOMEM);

    for (*surfaceCount = 0; *surfaceCount < ctx->max_surface_count; ++*surfaceCount) {
        res = nvenc_alloc_surface(avctx, *surfaceCount);
        if (res)
            return res;
    }

    return 0;
}

static av_cold int nvenc_setup_extradata(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    NVENCSTATUS nv_status;
    uint32_t outSize = 0;
    char tmpHeader[256];
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = { 0 };
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;

    payload.spsppsBuffer = tmpHeader;
    payload.inBufferSize = sizeof(tmpHeader);
    payload.outSPSPPSPayloadSize = &outSize;

    nv_status = p_nvenc->nvEncGetSequenceParams(ctx->nvencoder, &payload);
    if (nv_status != NV_ENC_SUCCESS) {
        return nvenc_print_error(avctx, nv_status, "GetSequenceParams failed");
    }

    avctx->extradata_size = outSize;
    avctx->extradata = av_mallocz(outSize + AV_INPUT_BUFFER_PADDING_SIZE);

    if (!avctx->extradata) {
        return AVERROR(ENOMEM);
    }

    memcpy(avctx->extradata, tmpHeader, outSize);

    return 0;
}

static av_cold int nvenc_encode_init(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    int res;
    int i;
    int surfaceCount = 0;

    if (!nvenc_dyload_nvenc(avctx))
        return AVERROR_EXTERNAL;

    res = nvenc_setup_device(avctx);
    if (res)
        goto error;

    res = nvenc_open_session(avctx);
    if (res)
        goto error;

    res = nvenc_setup_encoder(avctx);
    if (res)
        goto error;

    res = nvenc_setup_surfaces(avctx, &surfaceCount);
    if (res)
        goto error;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        res = nvenc_setup_extradata(avctx);
        if (res)
            goto error;
    }

    return 0;

error:
    av_fifo_freep(&ctx->timestamp_list);
    av_fifo_freep(&ctx->output_surface_ready_queue);
    av_fifo_freep(&ctx->output_surface_queue);

    for (i = 0; i < surfaceCount; ++i) {
        if (avctx->pix_fmt != AV_PIX_FMT_CUDA)
            p_nvenc->nvEncDestroyInputBuffer(ctx->nvencoder, ctx->surfaces[i].input_surface);
        av_frame_free(&ctx->surfaces[i].in_ref);
        p_nvenc->nvEncDestroyBitstreamBuffer(ctx->nvencoder, ctx->surfaces[i].output_surface);
    }
    av_freep(&ctx->surfaces);

    if (ctx->nvencoder)
        p_nvenc->nvEncDestroyEncoder(ctx->nvencoder);
    ctx->nvencoder = NULL;

    if (ctx->cu_context_internal)
        dl_fn->cu_ctx_destroy(ctx->cu_context_internal);
    ctx->cu_context = ctx->cu_context_internal = NULL;

    nvenc_unload_nvenc(avctx);

    return res;
}

static av_cold int nvenc_encode_close(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;
    int i;

    av_fifo_freep(&ctx->timestamp_list);
    av_fifo_freep(&ctx->output_surface_ready_queue);
    av_fifo_freep(&ctx->output_surface_queue);

    if (avctx->pix_fmt == AV_PIX_FMT_CUDA) {
        for (i = 0; i < ctx->max_surface_count; ++i) {
            if (ctx->surfaces[i].input_surface) {
                 p_nvenc->nvEncUnmapInputResource(ctx->nvencoder, ctx->surfaces[i].in_map.mappedResource);
            }
        }
        for (i = 0; i < ctx->nb_registered_frames; i++) {
            if (ctx->registered_frames[i].regptr)
                p_nvenc->nvEncUnregisterResource(ctx->nvencoder, ctx->registered_frames[i].regptr);
        }
        ctx->nb_registered_frames = 0;
    }

    for (i = 0; i < ctx->max_surface_count; ++i) {
        if (avctx->pix_fmt != AV_PIX_FMT_CUDA)
            p_nvenc->nvEncDestroyInputBuffer(ctx->nvencoder, ctx->surfaces[i].input_surface);
        av_frame_free(&ctx->surfaces[i].in_ref);
        p_nvenc->nvEncDestroyBitstreamBuffer(ctx->nvencoder, ctx->surfaces[i].output_surface);
    }
    av_freep(&ctx->surfaces);
    ctx->max_surface_count = 0;

    p_nvenc->nvEncDestroyEncoder(ctx->nvencoder);
    ctx->nvencoder = NULL;

    if (ctx->cu_context_internal)
        dl_fn->cu_ctx_destroy(ctx->cu_context_internal);
    ctx->cu_context = ctx->cu_context_internal = NULL;

    nvenc_unload_nvenc(avctx);

    return 0;
}

static NvencSurface *get_free_frame(NvencContext *ctx)
{
    int i;

    for (i = 0; i < ctx->max_surface_count; ++i) {
        if (!ctx->surfaces[i].lockCount) {
            ctx->surfaces[i].lockCount = 1;
            return &ctx->surfaces[i];
        }
    }

    return NULL;
}

static int nvenc_copy_frame(AVCodecContext *avctx, NvencSurface *inSurf,
            NV_ENC_LOCK_INPUT_BUFFER *lockBufferParams, const AVFrame *frame)
{
    uint8_t *buf = lockBufferParams->bufferDataPtr;
    int off = inSurf->height * lockBufferParams->pitch;

    if (frame->format == AV_PIX_FMT_YUV420P) {
        av_image_copy_plane(buf, lockBufferParams->pitch,
            frame->data[0], frame->linesize[0],
            avctx->width, avctx->height);

        buf += off;

        av_image_copy_plane(buf, lockBufferParams->pitch >> 1,
            frame->data[2], frame->linesize[2],
            avctx->width >> 1, avctx->height >> 1);

        buf += off >> 2;

        av_image_copy_plane(buf, lockBufferParams->pitch >> 1,
            frame->data[1], frame->linesize[1],
            avctx->width >> 1, avctx->height >> 1);
    } else if (frame->format == AV_PIX_FMT_NV12) {
        av_image_copy_plane(buf, lockBufferParams->pitch,
            frame->data[0], frame->linesize[0],
            avctx->width, avctx->height);

        buf += off;

        av_image_copy_plane(buf, lockBufferParams->pitch,
            frame->data[1], frame->linesize[1],
            avctx->width, avctx->height >> 1);
    } else if (frame->format == AV_PIX_FMT_YUV444P) {
        av_image_copy_plane(buf, lockBufferParams->pitch,
            frame->data[0], frame->linesize[0],
            avctx->width, avctx->height);

        buf += off;

        av_image_copy_plane(buf, lockBufferParams->pitch,
            frame->data[1], frame->linesize[1],
            avctx->width, avctx->height);

        buf += off;

        av_image_copy_plane(buf, lockBufferParams->pitch,
            frame->data[2], frame->linesize[2],
            avctx->width, avctx->height);
    } else {
        av_log(avctx, AV_LOG_FATAL, "Invalid pixel format!\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int nvenc_find_free_reg_resource(AVCodecContext *avctx)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    int i;

    if (ctx->nb_registered_frames == FF_ARRAY_ELEMS(ctx->registered_frames)) {
        for (i = 0; i < ctx->nb_registered_frames; i++) {
            if (!ctx->registered_frames[i].mapped) {
                if (ctx->registered_frames[i].regptr) {
                    p_nvenc->nvEncUnregisterResource(ctx->nvencoder,
                                                ctx->registered_frames[i].regptr);
                    ctx->registered_frames[i].regptr = NULL;
                }
                return i;
            }
        }
    } else {
        return ctx->nb_registered_frames++;
    }

    av_log(avctx, AV_LOG_ERROR, "Too many registered CUDA frames\n");
    return AVERROR(ENOMEM);
}

static int nvenc_register_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
    NV_ENC_REGISTER_RESOURCE reg;
    int i, idx, ret;

    for (i = 0; i < ctx->nb_registered_frames; i++) {
        if (ctx->registered_frames[i].ptr == (CUdeviceptr)frame->data[0])
            return i;
    }

    idx = nvenc_find_free_reg_resource(avctx);
    if (idx < 0)
        return idx;

    reg.version            = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType       = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.width              = frames_ctx->width;
    reg.height             = frames_ctx->height;
    reg.bufferFormat       = ctx->surfaces[0].format;
    reg.pitch              = frame->linesize[0];
    reg.resourceToRegister = frame->data[0];

    ret = p_nvenc->nvEncRegisterResource(ctx->nvencoder, &reg);
    if (ret != NV_ENC_SUCCESS) {
        nvenc_print_error(avctx, ret, "Error registering an input resource");
        return AVERROR_UNKNOWN;
    }

    ctx->registered_frames[idx].ptr    = (CUdeviceptr)frame->data[0];
    ctx->registered_frames[idx].regptr = reg.registeredResource;
    return idx;
}

static int nvenc_upload_frame(AVCodecContext *avctx, const AVFrame *frame,
                                      NvencSurface *nvenc_frame)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    int res;
    NVENCSTATUS nv_status;

    if (avctx->pix_fmt == AV_PIX_FMT_CUDA) {
        int reg_idx = nvenc_register_frame(avctx, frame);
        if (reg_idx < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not register an input CUDA frame\n");
            return reg_idx;
        }

        res = av_frame_ref(nvenc_frame->in_ref, frame);
        if (res < 0)
            return res;

        nvenc_frame->in_map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        nvenc_frame->in_map.registeredResource = ctx->registered_frames[reg_idx].regptr;
        nv_status = p_nvenc->nvEncMapInputResource(ctx->nvencoder, &nvenc_frame->in_map);
        if (nv_status != NV_ENC_SUCCESS) {
            av_frame_unref(nvenc_frame->in_ref);
            return nvenc_print_error(avctx, nv_status, "Error mapping an input resource");
        }

        ctx->registered_frames[reg_idx].mapped = 1;
        nvenc_frame->reg_idx                   = reg_idx;
        nvenc_frame->input_surface             = nvenc_frame->in_map.mappedResource;
        return 0;
    } else {
        NV_ENC_LOCK_INPUT_BUFFER lockBufferParams = { 0 };

        lockBufferParams.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockBufferParams.inputBuffer = nvenc_frame->input_surface;

        nv_status = p_nvenc->nvEncLockInputBuffer(ctx->nvencoder, &lockBufferParams);
        if (nv_status != NV_ENC_SUCCESS) {
            return nvenc_print_error(avctx, nv_status, "Failed locking nvenc input buffer");
        }

        res = nvenc_copy_frame(avctx, nvenc_frame, &lockBufferParams, frame);

        nv_status = p_nvenc->nvEncUnlockInputBuffer(ctx->nvencoder, nvenc_frame->input_surface);
        if (nv_status != NV_ENC_SUCCESS) {
            return nvenc_print_error(avctx, nv_status, "Failed unlocking input buffer!");
        }

        return res;
    }
}

static void nvenc_codec_specific_pic_params(AVCodecContext *avctx,
                                                    NV_ENC_PIC_PARAMS *params)
{
    NvencContext *ctx = avctx->priv_data;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
      params->codecPicParams.h264PicParams.sliceMode = ctx->encode_config.encodeCodecConfig.h264Config.sliceMode;
      params->codecPicParams.h264PicParams.sliceModeData = ctx->encode_config.encodeCodecConfig.h264Config.sliceModeData;
      break;
    case AV_CODEC_ID_H265:
      params->codecPicParams.hevcPicParams.sliceMode = ctx->encode_config.encodeCodecConfig.hevcConfig.sliceMode;
      params->codecPicParams.hevcPicParams.sliceModeData = ctx->encode_config.encodeCodecConfig.hevcConfig.sliceModeData;
      break;
    }
}

static int process_output_surface(AVCodecContext *avctx, AVPacket *pkt, NvencSurface *tmpoutsurf)
{
    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    uint32_t slice_mode_data;
    uint32_t *slice_offsets;
    NV_ENC_LOCK_BITSTREAM lock_params = { 0 };
    NVENCSTATUS nv_status;
    int res = 0;

    enum AVPictureType pict_type;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
      slice_mode_data = ctx->encode_config.encodeCodecConfig.h264Config.sliceModeData;
      break;
    case AV_CODEC_ID_H265:
      slice_mode_data = ctx->encode_config.encodeCodecConfig.hevcConfig.sliceModeData;
      break;
    default:
      av_log(avctx, AV_LOG_ERROR, "Unknown codec name\n");
      res = AVERROR(EINVAL);
      goto error;
    }
    slice_offsets = av_mallocz(slice_mode_data * sizeof(*slice_offsets));

    if (!slice_offsets)
        return AVERROR(ENOMEM);

    lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;

    lock_params.doNotWait = 0;
    lock_params.outputBitstream = tmpoutsurf->output_surface;
    lock_params.sliceOffsets = slice_offsets;

    nv_status = p_nvenc->nvEncLockBitstream(ctx->nvencoder, &lock_params);
    if (nv_status != NV_ENC_SUCCESS) {
        res = nvenc_print_error(avctx, nv_status, "Failed locking bitstream buffer");
        goto error;
    }

    if (res = ff_alloc_packet2(avctx, pkt, lock_params.bitstreamSizeInBytes,0)) {
        p_nvenc->nvEncUnlockBitstream(ctx->nvencoder, tmpoutsurf->output_surface);
        goto error;
    }

    memcpy(pkt->data, lock_params.bitstreamBufferPtr, lock_params.bitstreamSizeInBytes);

    nv_status = p_nvenc->nvEncUnlockBitstream(ctx->nvencoder, tmpoutsurf->output_surface);
    if (nv_status != NV_ENC_SUCCESS)
        nvenc_print_error(avctx, nv_status, "Failed unlocking bitstream buffer, expect the gates of mordor to open");


    if (avctx->pix_fmt == AV_PIX_FMT_CUDA) {
        p_nvenc->nvEncUnmapInputResource(ctx->nvencoder, tmpoutsurf->in_map.mappedResource);
        av_frame_unref(tmpoutsurf->in_ref);
        ctx->registered_frames[tmpoutsurf->reg_idx].mapped = 0;

        tmpoutsurf->input_surface = NULL;
    }

    switch (lock_params.pictureType) {
    case NV_ENC_PIC_TYPE_IDR:
        pkt->flags |= AV_PKT_FLAG_KEY;
    case NV_ENC_PIC_TYPE_I:
        pict_type = AV_PICTURE_TYPE_I;
        break;
    case NV_ENC_PIC_TYPE_P:
        pict_type = AV_PICTURE_TYPE_P;
        break;
    case NV_ENC_PIC_TYPE_B:
        pict_type = AV_PICTURE_TYPE_B;
        break;
    case NV_ENC_PIC_TYPE_BI:
        pict_type = AV_PICTURE_TYPE_BI;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unknown picture type encountered, expect the output to be broken.\n");
        av_log(avctx, AV_LOG_ERROR, "Please report this error and include as much information on how to reproduce it as possible.\n");
        res = AVERROR_EXTERNAL;
        goto error;
    }

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    avctx->coded_frame->pict_type = pict_type;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    ff_side_data_set_encoder_stats(pkt,
        (lock_params.frameAvgQP - 1) * FF_QP2LAMBDA, NULL, 0, pict_type);

    pkt->pts = lock_params.outputTimeStamp;
    pkt->dts = timestamp_queue_dequeue(ctx->timestamp_list);

    /* when there're b frame(s), set dts offset */
    if (ctx->encode_config.frameIntervalP >= 2)
        pkt->dts -= 1;

    if (pkt->dts > pkt->pts)
        pkt->dts = pkt->pts;

    if (ctx->last_dts != AV_NOPTS_VALUE && pkt->dts <= ctx->last_dts)
        pkt->dts = ctx->last_dts + 1;

    ctx->last_dts = pkt->dts;

    av_free(slice_offsets);

    return 0;

error:

    av_free(slice_offsets);
    timestamp_queue_dequeue(ctx->timestamp_list);

    return res;
}

static int output_ready(NvencContext *ctx, int flush)
{
    int nb_ready, nb_pending;

    nb_ready   = av_fifo_size(ctx->output_surface_ready_queue)   / sizeof(NvencSurface*);
    nb_pending = av_fifo_size(ctx->output_surface_queue)         / sizeof(NvencSurface*);
    return nb_ready > 0 && (flush || nb_ready + nb_pending >= ctx->buffer_delay);
}

static int nvenc_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
    const AVFrame *frame, int *got_packet)
{
    NVENCSTATUS nv_status;
    NvencSurface *tmpoutsurf, *inSurf;
    int res;

    NvencContext *ctx = avctx->priv_data;
    NvencDynLoadFunctions *dl_fn = &ctx->nvenc_dload_funcs;
    NV_ENCODE_API_FUNCTION_LIST *p_nvenc = &dl_fn->nvenc_funcs;

    NV_ENC_PIC_PARAMS pic_params = { 0 };
    pic_params.version = NV_ENC_PIC_PARAMS_VER;

    if (frame) {
        inSurf = get_free_frame(ctx);
        av_assert0(inSurf);

        res = nvenc_upload_frame(avctx, frame, inSurf);
        if (res) {
            inSurf->lockCount = 0;
            return res;
        }

        pic_params.inputBuffer = inSurf->input_surface;
        pic_params.bufferFmt = inSurf->format;
        pic_params.inputWidth = avctx->width;
        pic_params.inputHeight = avctx->height;
        pic_params.outputBitstream = inSurf->output_surface;
        pic_params.completionEvent = 0;

        if (avctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) {
            if (frame->top_field_first) {
                pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM;
            } else {
                pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP;
            }
        } else {
            pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        }

        pic_params.encodePicFlags = 0;
        pic_params.inputTimeStamp = frame->pts;
        pic_params.inputDuration = 0;

        nvenc_codec_specific_pic_params(avctx, &pic_params);

        timestamp_queue_enqueue(ctx->timestamp_list, frame->pts);
    } else {
        pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    }

    nv_status = p_nvenc->nvEncEncodePicture(ctx->nvencoder, &pic_params);

    if (frame && nv_status == NV_ENC_ERR_NEED_MORE_INPUT)
        av_fifo_generic_write(ctx->output_surface_queue, &inSurf, sizeof(inSurf), NULL);

    if (nv_status != NV_ENC_SUCCESS && nv_status != NV_ENC_ERR_NEED_MORE_INPUT) {
        return nvenc_print_error(avctx, nv_status, "EncodePicture failed!");
    }

    if (nv_status != NV_ENC_ERR_NEED_MORE_INPUT) {
        while (av_fifo_size(ctx->output_surface_queue) > 0) {
            av_fifo_generic_read(ctx->output_surface_queue, &tmpoutsurf, sizeof(tmpoutsurf), NULL);
            av_fifo_generic_write(ctx->output_surface_ready_queue, &tmpoutsurf, sizeof(tmpoutsurf), NULL);
        }

        if (frame)
            av_fifo_generic_write(ctx->output_surface_ready_queue, &inSurf, sizeof(inSurf), NULL);
    }

    if (output_ready(ctx, !frame)) {
        av_fifo_generic_read(ctx->output_surface_ready_queue, &tmpoutsurf, sizeof(tmpoutsurf), NULL);

        res = process_output_surface(avctx, pkt, tmpoutsurf);

        if (res)
            return res;

        av_assert0(tmpoutsurf->lockCount);
        tmpoutsurf->lockCount--;

        *got_packet = 1;
    } else {
        *got_packet = 0;
    }

    return 0;
}

static const enum AVPixelFormat pix_fmts_nvenc[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV444P,
#if CONFIG_CUDA
    AV_PIX_FMT_CUDA,
#endif
    AV_PIX_FMT_NONE
};

#define OFFSET(x) offsetof(NvencContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "preset", "Set the encoding preset (one of slow = hq 2pass, medium = hq, fast = hp, hq, hp, bd, ll, llhq, llhp, lossless, losslesshp, default)", OFFSET(preset), AV_OPT_TYPE_STRING, { .str = "medium" }, 0, 0, VE },
    { "profile", "Set the encoding profile (high, main, baseline or high444p)", OFFSET(profile), AV_OPT_TYPE_STRING, { .str = "main" }, 0, 0, VE },
    { "level", "Set the encoding level restriction (auto, 1.0, 1.0b, 1.1, 1.2, ..., 4.2, 5.0, 5.1)", OFFSET(level), AV_OPT_TYPE_STRING, { .str = "auto" }, 0, 0, VE },
    { "tier", "Set the encoding tier (main or high)", OFFSET(tier), AV_OPT_TYPE_STRING, { .str = "main" }, 0, 0, VE },
    { "cbr", "Use cbr encoding mode", OFFSET(cbr), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "2pass", "Use 2pass encoding mode", OFFSET(twopass), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, VE },
    { "gpu", "Selects which NVENC capable GPU to use. First GPU is 0, second is 1, and so on.", OFFSET(gpu), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    { "delay", "Delays frame output by the given amount of frames.", OFFSET(buffer_delay), AV_OPT_TYPE_INT, { .i64 = INT_MAX }, 0, INT_MAX, VE },
    { NULL }
};

static const AVCodecDefault nvenc_defaults[] = {
    { "b", "2M" },
    { "qmin", "-1" },
    { "qmax", "-1" },
    { "qdiff", "-1" },
    { "qblur", "-1" },
    { "qcomp", "-1" },
    { "g", "250" },
    { "bf", "0" },
    { NULL },
};

#if CONFIG_NVENC_ENCODER
static const AVClass nvenc_class = {
    .class_name = "nvenc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_nvenc_encoder = {
    .name = "nvenc",
    .long_name = NULL_IF_CONFIG_SMALL("NVIDIA NVENC h264 encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(NvencContext),
    .init = nvenc_encode_init,
    .encode2 = nvenc_encode_frame,
    .close = nvenc_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &nvenc_class,
    .defaults = nvenc_defaults,
    .pix_fmts = pix_fmts_nvenc,
};
#endif

/* Add an alias for nvenc_h264 */
#if CONFIG_NVENC_H264_ENCODER
static const AVClass nvenc_h264_class = {
    .class_name = "nvenc_h264",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_nvenc_h264_encoder = {
    .name = "nvenc_h264",
    .long_name = NULL_IF_CONFIG_SMALL("NVIDIA NVENC h264 encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(NvencContext),
    .init = nvenc_encode_init,
    .encode2 = nvenc_encode_frame,
    .close = nvenc_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &nvenc_h264_class,
    .defaults = nvenc_defaults,
    .pix_fmts = pix_fmts_nvenc,
};
#endif

#if CONFIG_NVENC_HEVC_ENCODER
static const AVClass nvenc_hevc_class = {
    .class_name = "nvenc_hevc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_nvenc_hevc_encoder = {
    .name = "nvenc_hevc",
    .long_name = NULL_IF_CONFIG_SMALL("NVIDIA NVENC hevc encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H265,
    .priv_data_size = sizeof(NvencContext),
    .init = nvenc_encode_init,
    .encode2 = nvenc_encode_frame,
    .close = nvenc_encode_close,
    .capabilities = AV_CODEC_CAP_DELAY,
    .priv_class = &nvenc_hevc_class,
    .defaults = nvenc_defaults,
    .pix_fmts = pix_fmts_nvenc,
};
#endif
