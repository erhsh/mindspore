/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "device/ascend/ascend_kernel_runtime.h"

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <exception>
#include <algorithm>

#include "device/ascend/ascend_device_address.h"
#include "utils/context/ms_context.h"
#include "device/ascend/profiling/profiling_manager.h"
#include "hccl/hcom.h"
#include "runtime/context.h"
#include "device/ascend/ascend_stream_assign.h"
#include "device/ascend/ascend_memory_allocator.h"
#include "framework/ge_runtime/model_runner.h"
#include "device/ascend/tasksink/task_generator.h"
#include "session/anf_runtime_algorithm.h"
#include "device/ascend/profiling/profiling_utils.h"
#include "kernel/tbe/tbe_utils.h"
#include "kernel/tbe/tbe_python_funcs.h"
#include "pre_activate/mem_reuse/mem_reuse_checker.h"

using mindspore::device::ascend::ProfilingManager;
using mindspore::device::ascend::ProfilingUtils;
using mindspore::device::ascend::tasksink::TaskGenerator;
using mindspore::kernel::tbe::TbeUtils;
using std::vector;

namespace mindspore {
namespace device {
namespace ascend {
static const uint64_t ASCEND_MEM_SIZE = 20;
static const uint64_t ASCEND_MEM_SIZE_BYTE = (ASCEND_MEM_SIZE << 30);
static const size_t PRAMATER_OUTPUT_INDEX = 0;

AscendKernelRuntime::~AscendKernelRuntime() { graph_model_map_.clear(); }

void AscendKernelRuntime::ClearGraphModelMap() {
  for (auto &iter : graph_model_id_map_) {
    MS_LOG(INFO) << "Ge UnloadModel " << iter.second;
    auto ret = ge::model_runner::ModelRunner::Instance().UnloadModel(iter.second);
    if (!ret) {
      MS_LOG(ERROR) << "UnloadModel failed";
    }
  }
}

bool AscendKernelRuntime::NeedDestroyHccl() {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (!context_ptr->enable_hccl()) {
    MS_LOG(INFO) << "hccl is not enabled";
    return false;
  }
  // Note: make sure hcom_connectivity_detection api never be used.
  return true;
}

void AscendKernelRuntime::ReleaseDeviceRes() {
  MS_LOG(INFO) << "ascend finalize start";
  // release ge runtime
  ClearGraphModelMap();

  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  auto ret = rtSetDevice(context_ptr->device_id());
  if (ret != RT_ERROR_NONE) {
    MS_EXCEPTION(DeviceProcessError) << "rtSetDevice, ret[" << static_cast<int>(ret) << "]";
  }

  FreeDeviceMemory();
  (void)DestroyHccl();
  (void)ResetDevice();
  (void)ProfilingManager::GetInstance().StopProfiling();
  MS_LOG(INFO) << "ascend finalize end";
}

bool AscendKernelRuntime::Init() {
  if (initialized_) {
    return true;
  }
  bool ret = false;
#ifdef ENABLE_DUMP_E2E
  ret = SetDumpConf();
  if (!ret) {
    MS_LOG(INFO) << "no dump conf to set!";
  }
#endif

  ret = InitDevice();
  if (!ret) {
    return ret;
  }

  ret = MallocDeviceMemory();
  if (!ret) {
    return ret;
  }

  ret = ProfilingManager::GetInstance().StartupProfiling(device_id_);
  if (!ret) {
    MS_EXCEPTION(DeviceProcessError) << "StartupProfiling failed.";
  }

  initialized_ = true;
  return ret;
}

#ifdef ENABLE_DUMP_E2E
namespace {
void DumpOutput(mindspore::session::KernelGraph *graph, const string &dump_path, DumpConfPtr dump_conf) {
  MS_EXCEPTION_IF_NULL(graph);
  MS_EXCEPTION_IF_NULL(dump_conf);
  bool trans_flag = dump_conf->trans_flag();
  const auto &apply_kernels = graph->execution_order();
  for (const auto &node : apply_kernels) {
    MS_EXCEPTION_IF_NULL(node);
    auto node_name = AnfAlgo::GetCNodeName(node);
    std::string kernel_name = node->fullname_with_scope();
    if (!dump_conf->IsKernelNeedDump(kernel_name)) {
      continue;
    }
    const std::string strsrc = "/";
    const std::string strdst = "--";
    std::string::size_type pos = 0;
    std::string::size_type srclen = strsrc.size();
    std::string::size_type dstlen = strdst.size();
    while ((pos = kernel_name.find(strsrc, pos)) != std::string::npos) {
      kernel_name.replace(pos, srclen, strdst);
      pos += dstlen;
    }
    auto output_size = AnfAlgo::GetOutputTensorNum(node);
    for (size_t j = 0; j < output_size; ++j) {
      auto addr = AnfAlgo::GetOutputAddr(node, j);
      auto shape = AnfAlgo::GetOutputDeviceShape(node, j);
      auto type = AnfAlgo::GetOutputDeviceDataType(node, j);
      auto format = AnfAlgo::GetOutputFormat(node, j);
      string filepath = dump_path + '/' + kernel_name + '_' + "output_" + std::to_string(j);
      auto ascend_addr = dynamic_cast<const mindspore::device::ascend::AscendDeviceAddress *>(addr);
      std::vector<int> int_shapes;
      (void)std::transform(shape.begin(), shape.end(), std::back_inserter(int_shapes),
                           [](size_t inner_item) { return SizeToInt(inner_item); });
      auto ret = ascend_addr->DumpMemToFile(trans_flag, filepath, format, int_shapes, type);
      if (!ret) {
        MS_LOG(ERROR) << "DumpMemToFile Failed: flag:" << trans_flag << ", path:" << filepath
                      << ", host_format:" << format << ".!";
      }
    }
  }
}

void DumpParameters(mindspore::session::KernelGraph *graph, const string &dump_path, DumpConfPtr dump_conf) {
  MS_EXCEPTION_IF_NULL(graph);
  MS_EXCEPTION_IF_NULL(dump_conf);
  bool trans_flag = dump_conf->trans_flag();
  const auto &parameters = graph->inputs();
  for (auto &item : parameters) {
    if (!item->isa<Parameter>()) {
      continue;
    }
    std::string parameter_name = item->fullname_with_scope();
    if (!dump_conf->IsKernelNeedDump(parameter_name)) {
      continue;
    }
    auto addr = AnfAlgo::GetOutputAddr(item, PRAMATER_OUTPUT_INDEX);
    auto shape = AnfAlgo::GetOutputDeviceShape(item, PRAMATER_OUTPUT_INDEX);
    auto type = AnfAlgo::GetOutputDeviceDataType(item, PRAMATER_OUTPUT_INDEX);
    auto format = AnfAlgo::GetOutputFormat(item, PRAMATER_OUTPUT_INDEX);
    string filepath = dump_path + '/' + parameter_name + '_' + "output_0";
    auto ascend_addr = dynamic_cast<const mindspore::device::ascend::AscendDeviceAddress *>(addr);
    std::vector<int> int_shapes;
    (void)std::transform(shape.begin(), shape.end(), std::back_inserter(int_shapes),
                         [](size_t inner_item) { return SizeToInt(inner_item); });
    auto ret = ascend_addr->DumpMemToFile(trans_flag, filepath, format, int_shapes, type);
    if (!ret) {
      MS_LOG(ERROR) << "DumpMemToFile Failed: flag:" << trans_flag << ", path:" << filepath
                    << ", host_format:" << format << ".!";
    }
  }
}
}  // namespace
#endif

bool AscendKernelRuntime::DumpData(mindspore::session::KernelGraph *graph) {
  MS_EXCEPTION_IF_NULL(graph);
#ifdef ENABLE_DUMP_E2E
  MS_LOG(INFO) << "start dump step";
  DumpConfPtr dump_conf = GetDumpConf();
  MS_EXCEPTION_IF_NULL(dump_conf);
  dump_conf->UpdataCurIter();
  bool dump_flag = dump_conf->dump_enable();
  if (!dump_flag) {
    MS_LOG(INFO) << "dump flag is disable, pass dump step";
    return true;
  }
  uint32_t cur_iter = dump_conf->cur_iter();
  if (dump_conf->dump_iter() != 0) {
    if (cur_iter != dump_conf->dump_iter()) {
      return true;
    }
  }
  MS_LOG(INFO) << "cur iter is " << cur_iter;
  std::string net_name = dump_conf->dump_net_name();
  std::string iterator = to_string(cur_iter);
  std::string dump_path = dump_conf->dump_path();
  if (dump_path.back() == '/') {
    dump_path = dump_path + net_name + '/' + iterator;
  } else {
    dump_path = dump_path + '/' + net_name + '/' + iterator;
  }
  // dump output
  DumpOutput(graph, dump_path, dump_conf);
  // dump parameters
  DumpParameters(graph, dump_path, dump_conf);
#endif
  return true;
}

DeviceAddressPtr AscendKernelRuntime::CreateDeviceAddress(void *device_ptr, size_t device_size, const string &format,
                                                          TypeId type_id) {
  return std::make_shared<AscendDeviceAddress>(device_ptr, device_size, format, type_id);
}

void AscendKernelRuntime::MallocOpMemory(const DeviceAddressPtr address, size_t size, int flag) {
  MS_EXCEPTION_IF_NULL(MsContext::GetInstance());
  if (MsContext::GetInstance()->enable_dynamic_mem_pool()) {
    auto device_ptr = AscendMemoryAllocator::GetInstance().AllocTensorMem(size);
    MS_EXCEPTION_IF_NULL(device_ptr);
    address->ptr_ = device_ptr;
    address->mem_dynamic_alloc_ = true;
    return;
  }
  if (flag == kStaticMem) {
    address->ptr_ = MallocStaticMem(size, false);
  } else if (flag == kDynamicMem) {
    address->ptr_ = MallocDynamicMem(size, false);
  } else {
    MS_LOG(EXCEPTION) << "Unknown memory type!";
  }
}

bool AscendKernelRuntime::GenTask(const session::KernelGraph *graph) {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool is_task_sink = context_ptr->enable_task_sink();
  if (!is_task_sink) {
    return true;
  }
#ifdef MEM_REUSE_DEBUG
  if (!context_ptr->enable_mem_reuse()) {
    // Get normal graph ir for memreuse
    mindspore::memreuse::MemReuseChecker::GetInstance().CheckNormalIR(graph);
  }
#endif
  if (graph == nullptr) {
    MS_EXCEPTION(NotExistsError) << "session::KernelGraph is NULL!";
  }
  vector<std::shared_ptr<TaskInfo>> task_info_list;
  auto anf_node_list = graph->execution_order();
  TaskGenerator::GenTasks(anf_node_list, &task_info_list, graph->graph_id());

  AscendStreamAssign &assign_instance = AscendStreamAssign::GetInstance();
  // the streams' flag not HEAD_STREAM
  std::vector<uint32_t> wait_active_stream_list = assign_instance.GetWaitStreams();
  std::vector<uint32_t> force_copy_stream_list = assign_instance.GetHcomStreams();

  MS_LOG(INFO) << "call DavinciModel total stream num:" << assign_instance.GetTotalStreamNum()
               << ", total event num:" << assign_instance.GetTotalEventNum()
               << ", wait_active_stream_list size:" << wait_active_stream_list.size()
               << ", force_copy_stream_list size:" << force_copy_stream_list.size();

  std::vector<std::shared_ptr<ge::model_runner::OpInfo>> empty_list;
  std::shared_ptr<ge::model_runner::DavinciModel> model = std::make_shared<ge::model_runner::DavinciModel>(
    task_info_list, empty_list, empty_list, empty_list, empty_list, wait_active_stream_list, force_copy_stream_list, 0,
    0, 0, 0, 0, 0, assign_instance.GetTotalStreamNum(), 1, assign_instance.GetTotalEventNum(), 0);

  graph_model_map_[graph] = model;
  graph_model_id_map_[graph] = graph->graph_id();
  MS_LOG(INFO) << "TaskGenerator GetTaskInfo end...";

  // Store the task_info_list
  task_map_.insert(std::make_pair(graph, task_info_list));

  return true;
}

uint32_t AscendKernelRuntime::GetGraphModelId(const session::KernelGraph *kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto iter = graph_model_id_map_.find(kernel_graph);
  if (iter == graph_model_id_map_.end()) {
    MS_LOG(EXCEPTION) << "graph not in the map";
  }
  return iter->second;
}

bool AscendKernelRuntime::LoadTask(const session::KernelGraph *graph) {
  if (graph == nullptr) {
    MS_EXCEPTION(NotExistsError) << "Null pointer graph, LoadTask failed. ";
  }
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool is_task_sink = context_ptr->enable_task_sink();
  if (!is_task_sink) {
    return true;
  }

  auto task_iter = graph_model_map_.find(graph);
  if (task_iter == graph_model_map_.end()) {
    MS_LOG(ERROR) << "task not exist";
    return false;
  }

  auto model_id = GetGraphModelId(graph);
  std::shared_ptr<ge::ModelListener> listener;
  MS_LOG(INFO) << "LoadDavinciModel mode_id:" << model_id;
  bool status =
    ge::model_runner::ModelRunner::Instance().LoadDavinciModel(device_id_, 0, model_id, task_iter->second, listener);
  if (!status) {
    MS_LOG(INFO) << "load task failed";
    return false;
  }
  if (ProfilingManager::GetInstance().IsProfiling()) {
    std::vector<uint32_t> task_ids = ge::model_runner::ModelRunner::Instance().GetTaskIdList(model_id);
    ProfilingUtils::ReportProfilingData(graph->graph_id(), task_ids);
  }
  return true;
}

bool AscendKernelRuntime::RunTask(const session::KernelGraph *graph) {
  MS_EXCEPTION_IF_NULL(graph);
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  ge::InputData input_tensors = ge::InputData();
  ge::OutputData *output_tensors = nullptr;
  auto model_id = GetGraphModelId(graph);
  bool status = ge::model_runner::ModelRunner::Instance().RunModel(model_id, input_tensors, output_tensors);
  if (!status) {
    MS_LOG(INFO) << "run task failed";
    return false;
  }
  return true;
}

bool AscendKernelRuntime::SyncStream() {
  if (RT_ERROR_NONE != rtStreamSynchronize(stream_)) {  // o for switch stream
    MS_LOG(ERROR) << "Call runtime rtStreamSynchronize error.";
    return false;
  }
  return true;
}

bool AscendKernelRuntime::InitDevice() {
  int device_count = 0;
  auto ret = rtGetDeviceCount(&device_count);
  if (ret != RT_ERROR_NONE) {
    MS_EXCEPTION(DeviceProcessError) << "rtGetDeviceCount, ret[" << static_cast<int>(ret) << "]";
  }

  ret = rtSetDevice(device_id_);
  if (ret != RT_ERROR_NONE) {
    MS_EXCEPTION(DeviceProcessError) << "rtSetDevice, ret[" << static_cast<int>(ret) << "]";
  }

  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (context_ptr == nullptr) {
    MS_LOG(ERROR) << "get MsContext instance failed";
    return false;
  }
  if (context_ptr->enable_hccl()) {
    if (!HcclInit()) {
      MS_LOG(ERROR) << "HcclInit init failed";
      return false;
    }
  }

  ret = rtCtxCreate(&rt_context_, 0, device_id_);
  if (ret != RT_ERROR_NONE) {
    MS_EXCEPTION(DeviceProcessError) << "rtCtxCreate, ret[" << static_cast<int>(ret) << "]";
  }

  ret = rtCtxSetCurrent(rt_context_);
  if (ret != RT_ERROR_NONE) {
    MS_EXCEPTION(DeviceProcessError) << "rtCtxSetCurrent, ret[" << ret << "]";
  }

  ret = rtStreamCreate(&stream_, 0);
  if (ret != RT_ERROR_NONE) {
    MS_LOG(EXCEPTION) << "rtStreamCreate, ret[" << ret << "]";
  }

  return true;
}

bool AscendKernelRuntime::ResetDevice() {
  auto ret = rtCtxSetCurrent(rt_context_);
  if (ret != RT_ERROR_NONE) {
    MS_LOG(ERROR) << "call rtCtxSetCurrent failed";
    return false;
  }

  if (stream_ != nullptr) {
    ret = rtStreamDestroy(stream_);
    if (ret != RT_ERROR_NONE) {
      MS_LOG(EXCEPTION) << "rtStreamDestroy, ret[" << ret << "]";
    }
    stream_ = nullptr;
  }

  if (rt_context_ != nullptr) {
    ret = rtCtxDestroy(rt_context_);
    if (ret != RT_ERROR_NONE) {
      MS_EXCEPTION(DeviceProcessError) << "rtCtxDestroy, ret[" << ret << "]";
    }
    rt_context_ = nullptr;
  }
  return true;
}

bool AscendKernelRuntime::HcclInit() {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (!context_ptr->IsTsdOpened()) {
    MS_LOG(EXCEPTION) << "Hccl dependent tsd is not open";
  }

  MS_LOG(INFO) << "do hcom init";
  std::string path;
  const char *config_path_str = std::getenv("MINDSPORE_HCCL_CONFIG_PATH");
  if (config_path_str == nullptr) {
    MS_LOG(ERROR) << "get hccl json config failed, please set env MINDSPORE_HCCL_CONFIG_PATH";
    return false;
  }
  path = config_path_str;
  char fullPath[PATH_MAX] = {0};
  if (path.size() > PATH_MAX || realpath(path.c_str(), fullPath) == nullptr) {
    MS_LOG(ERROR) << "file " << path << " is not exist";
    return false;
  }
  const char *identify = std::getenv("RANK_ID");
  if (identify == nullptr) {
    MS_LOG(ERROR) << "get hccl rankid failed, please set env RANK_ID";
    return false;
  }
  MS_LOG(INFO) << "MINDSPORE_HCCL_CONFIG_PATH : " << fullPath << ", RANK_ID: " << identify;
  hcclResult_t res = hcom_init(fullPath, identify);
  if (res != HCCL_SUCCESS) {
    MS_LOG(ERROR) << "hcom init failed, res is " << static_cast<int>(res);
    return false;
  }
  return true;
}

bool AscendKernelRuntime::DestroyHccl() {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (!NeedDestroyHccl()) {
    MS_LOG(INFO) << "hccl is not enable, no need to close.";
    return true;
  }
  hcclResult_t res = hcom_destroy();
  if (res != HCCL_SUCCESS) {
    MS_LOG(ERROR) << "hccl destroy failed";
    return false;
  }
  MS_LOG(INFO) << "hccl destroy successful, status = " << res << ".";
  context_ptr->set_enable_hccl(false);
  return true;
}

bool AscendKernelRuntime::MallocDeviceMemory() {
  device_mem_size_ = ASCEND_MEM_SIZE_BYTE;
  MS_EXCEPTION_IF_NULL(MsContext::GetInstance());
  if (MsContext::GetInstance()->enable_dynamic_mem_pool()) {
    static_mem_offset_ = FloatToSize(device_mem_size_ * GRAPH_INIT_DAVINCI_MEM_RATIO);
    device_mem_pool_size_ = FloatToSize(device_mem_size_ * (1 - GRAPH_INIT_DAVINCI_MEM_RATIO));
    auto ret = rtMalloc(reinterpret_cast<void **>(&device_mem_pool_base_), device_mem_pool_size_, RT_MEMORY_HBM);
    if (ret != RT_ERROR_NONE) {
      MS_EXCEPTION(DeviceProcessError) << "rtMalloc mem size[" << device_mem_pool_size_ << "] fail, ret[" << ret << "]";
    }
    AscendMemoryAllocator::GetInstance().set_device_mem_pool_base(device_mem_pool_base_);
    AscendMemoryAllocator::GetInstance().set_device_mem_pool_size(device_mem_pool_size_);
  } else {
    static_mem_offset_ = device_mem_size_;
  }
  auto ret = rtMalloc(reinterpret_cast<void **>(&device_mem_base_), device_mem_size_, RT_MEMORY_HBM);
  if (ret != RT_ERROR_NONE) {
    MS_EXCEPTION(DeviceProcessError) << "rtMalloc mem size[" << device_mem_size_ << "] fail, ret[" << ret << "]";
  }
  return true;
}

void AscendKernelRuntime::FreeDeviceMemory() {
  if (device_mem_base_ != nullptr) {
    auto ret = rtFree(device_mem_base_);
    if (ret != RT_ERROR_NONE) {
      MS_LOG(ERROR) << "rtFree mem size[" << device_mem_size_ << "] fail, ret[" << ret << "]";
    }
    device_mem_base_ = nullptr;
  }
  if (device_mem_pool_base_ != nullptr) {
    auto ret = rtFree(device_mem_pool_base_);
    if (ret != RT_ERROR_NONE) {
      MS_LOG(ERROR) << "rtFree mem size[" << device_mem_pool_size_ << "] fail, ret[" << ret << "]";
    }
    device_mem_pool_base_ = nullptr;
  }
}

void AscendKernelRuntime::FreeHostMemory() { dynamic_mem_offset_ = 0; }
}  // namespace ascend
}  // namespace device
}  // namespace mindspore