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

#include "resource_manager.h"
#include "backends/cuda/sampler.h"
#include "utils/utils.h"

#include "ppl/nn/models/onnx/runtime_builder.h"
#include "ppl/nn/models/onnx/runtime_builder_factory.h"
#include "ppl/common/cuda/cuda_env.h"

#ifdef PPLNN_ENABLE_PMX_MODEL
#include "ppl/nn/models/pmx/runtime_builder_factory.h"
#include "ppl/nn/models/pmx/load_model_options.h"
#include "ppl/nn/models/pmx/save_model_options.h"
#endif

#include <map>

using namespace ppl::common;
using namespace ppl::nn;
using namespace std;

namespace ppl { namespace llm { namespace cuda {

static const map<int, int32_t> g_cache_int2size = {
    {0, sizeof(float16_t)}, {8, sizeof(int8_t)}
};

#ifdef PPLNN_CUDA_ENABLE_NCCL
#define NCCL_CHECK(cmd, emsg)                                                \
    do {                                                                     \
        ncclResult_t e = (cmd);                                              \
        if (e != ncclSuccess) {                                              \
            LOG(ERROR) << "NCCL error(code:" << (int)e << ") on " << (emsg); \
            return RC_OTHER_ERROR;                                           \
        }                                                                    \
    } while (0);

static RetCode InitNccl(uint32_t tensor_parallel_size, std::vector<ncclComm_t>* nccl_comm_list) {
    nccl_comm_list->resize(tensor_parallel_size);
    std::vector<int> dev_list(tensor_parallel_size);
    for (size_t i = 0; i < tensor_parallel_size; ++i) {
        dev_list[i] = i;
    }
    NCCL_CHECK(ncclCommInitAll(nccl_comm_list->data(), tensor_parallel_size, dev_list.data()), "ncclCommInitAll");
    return RC_SUCCESS;
}
#endif

static Engine* CreateCudaEngine(int device_id, const std::string& quant_method, cudaStream_t stream) {
    ppl::nn::llm::cuda::EngineOptions options;
    options.device_id = device_id;
    options.mm_policy = ppl::nn::llm::cuda::MM_COMPACT;
    options.runtime_stream = stream;

    if (quant_method == "none") {
        options.quant_method = ppl::nn::llm::cuda::QUANT_METHOD_NONE;
    } else if (quant_method == "online_i8i8") {
        options.quant_method = ppl::nn::llm::cuda::QUANT_METHOD_ONLINE_I8I8;
    } else {
        LOG(ERROR) << "unknown/unsupported --quant-method option: " << quant_method;
        return nullptr;
    }

    return ppl::nn::llm::cuda::EngineFactory::Create(options);
}

static Runtime* CreatePPLRuntime(Engine* cuda_engine, const string& model_file) {
    auto builder = unique_ptr<onnx::RuntimeBuilder>(onnx::RuntimeBuilderFactory::Create());
    if (!builder) {
        LOG(ERROR) << "create onnx builder failed.";
        return nullptr;
    }

    auto rc = builder->LoadModel(model_file.c_str());
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "load model [" << model_file << "] failed: " << GetRetCodeStr(rc);
        return nullptr;
    }

    onnx::RuntimeBuilder::Resources resources;
    resources.engines = &cuda_engine;
    resources.engine_num = 1;

    rc = builder->SetResources(resources);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "set resources for builder failed: " << GetRetCodeStr(rc);
        return nullptr;
    }

    rc = builder->Preprocess();
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "builder preprocess failed: " << GetRetCodeStr(rc);
        return nullptr;
    }

    return builder->CreateRuntime();
}

#ifdef PPLNN_ENABLE_PMX_MODEL
static ppl::nn::Runtime* CreatePMXPPLRuntime(ppl::nn::Engine* cuda_engine, const std::string& model_file) {
    auto builder = std::unique_ptr<ppl::nn::pmx::RuntimeBuilder>(ppl::nn::pmx::RuntimeBuilderFactory::Create());
    if (!builder) {
        LOG(ERROR) << "create PmxRuntimeBuilder failed.";
        return nullptr;
    }

    ppl::nn::pmx::RuntimeBuilder::Resources resources;
    resources.engines = &cuda_engine;
    resources.engine_num = 1;

    std::string external_data_dir_fix;
    ppl::nn::pmx::LoadModelOptions opt;
    auto status = builder->LoadModel(model_file.c_str(), resources, opt);
    if (status != ppl::common::RC_SUCCESS) {
        LOG(ERROR) << "PmxRuntimeBuilder LoadModel failed: " << ppl::common::GetRetCodeStr(status);
        return nullptr;
    }
    
    status = builder->Preprocess();
    if (status != ppl::common::RC_SUCCESS) {
        LOG(ERROR) << "pmx preprocess failed: " << ppl::common::GetRetCodeStr(status);
        return nullptr;
    }

    return builder->CreateRuntime();
}
#endif //PPLNN_ENABLE_PMX_MODEL

static void StreamDeleter(void* s) {
    cudaStreamDestroy((cudaStream_t)s);
}

class InitTask final {
public:
    InitTask(uint32_t id, const string& model_dir, int use_pmx, uint64_t kv_cache_block_bytes, uint64_t kv_scale_block_bytes,
             float kv_cache_max_tokens_scale, const string& quant_method, Barrier* alloc_max_mem_barrier,
             CudaResourceManager* mgr)
        : id_(id)
        , model_dir_(model_dir)
        , use_pmx_(use_pmx)
        , kv_cache_block_bytes_(kv_cache_block_bytes)
        , kv_scale_block_bytes_(kv_scale_block_bytes)
        , kv_cache_max_tokens_scale_(kv_cache_max_tokens_scale)
        , quant_method_(quant_method)
        , alloc_max_mem_barrier_(alloc_max_mem_barrier)
        , mgr_(mgr) {}

    RetCode Process() {
        auto rc = InitCudaEnv(id_);
        if (rc != RC_SUCCESS) {
            LOG(ERROR) << "InitCudaEnv for device [" << id_ << "] failed.";
            return rc;
        }

        cudaStream_t stream;
        auto cu_ret = cudaStreamCreate(&stream);
        if (cu_ret != cudaSuccess) {
            LOG(ERROR) << "cudaStreamCreate failed: " << cudaGetErrorString(cu_ret);
            return RC_DEVICE_RUNTIME_ERROR;
        }
        unique_ptr<void, void(*)(void*)> __stream_guard(stream, StreamDeleter);

        auto engine = unique_ptr<Engine>(CreateCudaEngine(id_, quant_method_, stream));
        if (!engine) {
            LOG(ERROR) << "create cuda engine [" << id_ << "] failed.";
            return RC_OTHER_ERROR;
        }

#ifdef PPLNN_CUDA_ENABLE_NCCL
        rc = engine->Configure(ppl::nn::llm::cuda::ENGINE_CONF_SET_TP_NCCL_COMM, mgr_->nccl_comm_list[id_]);
        if (rc != RC_SUCCESS) {
            LOG(ERROR) << "engine configure nccl error";
            return RC_OTHER_ERROR;
        }
#endif
        LOG(INFO) << "create engine [" << id_ << "] success.";

        ppl::nn::llm::cuda::DeviceOptions dev_options;
        dev_options.mm_policy = ppl::nn::llm::cuda::MM_COMPACT;
        dev_options.device_id = id_;
        dev_options.stream = stream;

        unique_ptr<ppl::nn::DeviceContext> input_output_device(
            ppl::nn::llm::cuda::EngineFactory::CreateDeviceContext(dev_options));
        if (!input_output_device) {
            LOG(ERROR) << "create device for input/output failed: ";
            return RC_DEVICE_RUNTIME_ERROR;
        }

        unique_ptr<ppl::nn::DeviceContext> host_device(
            ppl::nn::llm::cuda::EngineFactory::CreateHostDeviceContext(
                ppl::nn::llm::cuda::HostDeviceOptions()));
        if (!host_device) {
            LOG(ERROR) << "create host device failed.";
            return RC_OUT_OF_MEMORY;
        }

        unique_ptr<Runtime> runtime;
        // TODO load models one by one to reduce memory usage
        {
#ifndef PPLNN_ENABLE_PMX_MODEL
            if (use_pmx_) {
                LOG(ERROR) << "enable PPLNN_ENABLE_PMX_MODEL option to use pmx model.";
                return RC_OTHER_ERROR;
            }
#endif

#ifdef PPLNN_ENABLE_PMX_MODEL
            if (use_pmx_) 
            {
                const string model_path = model_dir_ + "/model_slice_" + std::to_string(id_) + "/model.pmx";
                LOG(INFO) << "model_slice_" << std::to_string(id_) << ": " << model_path;
                runtime = unique_ptr<Runtime>(CreatePMXPPLRuntime(engine.get(), model_path));
            } 
            else 
#endif
            {
                const string model_path = model_dir_ + "/model_slice_" + std::to_string(id_) + "/model.onnx";
                LOG(INFO) << "model_slice_" << std::to_string(id_) << ": " << model_path;
                runtime = unique_ptr<Runtime>(CreatePPLRuntime(engine.get(), model_path));
            }
            
            if (!runtime) {
                LOG(ERROR) << "create runtime [" << id_ << "] failed.";
                return RC_OTHER_ERROR;
            }

            for (uint32_t i = 0; i < runtime->GetInputCount(); ++i) {
                auto tensor = runtime->GetInputTensor(i);
                tensor->SetDeviceContext(input_output_device.get());
            }
            for (uint32_t i = 0; i < runtime->GetOutputCount(); ++i) {
                auto tensor = runtime->GetOutputTensor(i);
                tensor->SetDeviceContext(input_output_device.get());
            }
        }

        {
            InferRuntimeParam param;
            param.stream = stream;
            param.engine = std::move(engine);
            param.input_output_device = std::move(input_output_device);
            mgr_->runtime_param_list[id_] = std::move(param);
        }

        if (id_ == 0) {
            size_t avail_bytes = 0, total = 0;
            cudaMemGetInfo(&avail_bytes, &total);
            const uint64_t kv_cache_max_bytes = kv_cache_max_tokens_scale_ * avail_bytes * (kv_cache_block_bytes_) /
                (kv_cache_block_bytes_ + kv_scale_block_bytes_);
            const uint64_t kv_scale_max_bytes = kv_cache_max_tokens_scale_ * avail_bytes * (kv_scale_block_bytes_) /
                (kv_cache_block_bytes_ + kv_scale_block_bytes_);
            LOG(INFO) << "avail_bytes: " << avail_bytes;
            LOG(INFO) << "kv_cache_max_bytes: " << kv_cache_max_bytes;
            LOG(INFO) << "kv_scale_max_bytes: " << kv_scale_max_bytes;

            mgr_->kv_cache_max_tokens = kv_cache_max_bytes / kv_cache_block_bytes_;
            LOG(INFO) << "max_tokens: " << mgr_->kv_cache_max_tokens;
        }

        alloc_max_mem_barrier_->Wait();

        ResourceItem item;

        cu_ret = cudaMalloc(&item.kv_cache_mem, mgr_->kv_cache_max_tokens * kv_cache_block_bytes_);
        if (cu_ret != cudaSuccess) {
            LOG(ERROR) << "alloc kv cache [" << mgr_->kv_cache_max_tokens * kv_cache_block_bytes_
                       << "] failed: " << cudaGetErrorString(cu_ret);
            return RC_OTHER_ERROR;
        }
        if (kv_scale_block_bytes_ > 0) {
            cu_ret = cudaMalloc(&item.kv_scale_mem, mgr_->kv_cache_max_tokens * kv_scale_block_bytes_);
            if (cu_ret != cudaSuccess) {
                cudaFree(item.kv_cache_mem);
                LOG(ERROR) << "alloc kv scale [" << mgr_->kv_cache_max_tokens * kv_scale_block_bytes_
                           << "] failed: " << cudaGetErrorString(cu_ret);
                return RC_OTHER_ERROR;
            }
        }

        item.runtime = runtime.release();
        item.host_device = host_device.release();

        mgr_->items[id_] = item;

        __stream_guard.release();
        return RC_SUCCESS;
    }

private:
    const uint32_t id_;
    const string& model_dir_;
    const bool use_pmx_;
    const uint64_t kv_cache_block_bytes_;
    const uint64_t kv_scale_block_bytes_;
    const float kv_cache_max_tokens_scale_;
    const string& quant_method_;
    Barrier* alloc_max_mem_barrier_;
    CudaResourceManager* mgr_;
};

std::unique_ptr<ppl::llm::utils::Sampler> CudaResourceManager::CreateCudaSampler(Runtime* runtime) {
    DeviceContext::Type needed_type;
    *((int64_t*)needed_type.str) = 0;
    needed_type.str[0] = 'c';
    needed_type.str[1] = 'u';
    needed_type.str[2] = 'd';
    needed_type.str[3] = 'a';

    DeviceContext* dev = nullptr;
    for (uint32_t i = 0; i < runtime->GetDeviceContextCount(); ++i) {
        if (runtime->GetDeviceContext(i)->GetType() == needed_type) {
            dev = runtime->GetDeviceContext(i);
            break;
        }
    }

    if (!dev) {
        LOG(ERROR) << "cannot find cuda device in runtime.";
        return std::unique_ptr<ppl::llm::utils::Sampler>();
    }

    cudaStream_t stream;
    auto rc = dev->Configure(ppl::nn::llm::cuda::DEV_CONF_GET_STREAM, &stream);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "Configure ppl::nn::llm::cuda::DEV_CONF_GET_STREAM failed: " << GetRetCodeStr(rc);
        return std::unique_ptr<ppl::llm::utils::Sampler>();
    }

    return std::make_unique<Sampler>(stream);
}

RetCode CudaResourceManager::Init(const ModelConfig& model_config, const ServerConfig& server_config) {
    auto size_iter = g_cache_int2size.find(model_config.cache_quant_bit);
    if (size_iter == g_cache_int2size.end()) {
        LOG(ERROR) << "no supported cache quant bit: [" << model_config.cache_quant_bit << "]";
        return RC_OTHER_ERROR;
    }
    int32_t size_cache_datatype = size_iter->second;

    const uint64_t kv_cache_block_bytes = model_config.num_layers * 2 * model_config.num_kv_heads /
        server_config.tensor_parallel_size * model_config.hidden_dim / model_config.num_heads * size_cache_datatype;
    uint64_t kv_scale_block_bytes = 0;
    if (model_config.cache_quant_bit > 0) {
        kv_scale_block_bytes = model_config.num_layers * 2 * model_config.num_kv_heads /
            server_config.tensor_parallel_size * model_config.hidden_dim / model_config.num_heads /
            model_config.cache_quant_group * sizeof(float16_t);
    }
    const int tensor_parallel_size = server_config.tensor_parallel_size;

    RetCode rc;
#ifdef PPLNN_CUDA_ENABLE_NCCL
    rc = InitNccl(tensor_parallel_size, &nccl_comm_list);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "NCCL init failed.";
        return rc;
    }
    LOG(INFO) << "Init Nccl successed";
#else
    if (server_config.tensor_parallel_size > 1) {
        LOG(ERROR) << "tensor_parallel_size > 1 need nccl support. Please compile with marco -DPPLNN_CUDA_ENABLE_NCCL=ON";
        return RC_OTHER_ERROR;
    }
#endif

    this->runtime_param_list.resize(tensor_parallel_size);
    this->items.resize(tensor_parallel_size);

    rc = this->device_worker_pool.Init(tensor_parallel_size);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "init device worker failed.";
        return rc;
    }

    Barrier alloc_max_mem_barrier;
    alloc_max_mem_barrier.Reset(tensor_parallel_size);
    rc = ppl::llm::utils::ParallelExecute<InitTask>(&this->device_worker_pool, server_config.model_dir, 
                                                    server_config.use_pmx,
                                                    kv_cache_block_bytes, kv_scale_block_bytes,
                                                    server_config.max_tokens_scale,
                                                    server_config.quant_method,
                                                    &alloc_max_mem_barrier, this);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "ParallelExecute(InitTask) failed.";
        return rc;
    }

    this->sampler = CreateCudaSampler(this->items[0].runtime);
    if (!sampler) {
        LOG(ERROR) << "CreateCudaSampler failed";
        return RC_OTHER_ERROR;
    }
    return RC_SUCCESS;
}

}}} // namespace ppl::llm::cuda
