/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/runtime/address_computation_thunk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "llvm/ADT/STLExtras.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/gpu/buffer_allocations.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/runtime/sequential_thunk.h"
#include "xla/service/gpu/thunk.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/status_macros.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/memory_allocation.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

AddressComputationThunk::AddressComputationThunk(
    ThunkInfo thunk_info, std::unique_ptr<ThunkSequence> embedded_thunk,
    std::vector<std::optional<const BufferAllocation::Slice>> operands,
    std::vector<std::optional<const BufferAllocation::Slice>> results,
    std::vector<std::optional<const BufferAllocation::Slice>>
        operand_offset_buffer_indices,
    std::vector<std::optional<const Shape>> operand_orig_shapes,
    std::vector<std::optional<const Shape>> operand_sliced_shapes,
    std::vector<std::optional<const BufferAllocation::Slice>>
        result_offset_buffer_indices,
    std::vector<std::optional<const Shape>> result_orig_shapes,
    std::vector<std::optional<const Shape>> result_sliced_shapes)
    : Thunk(Kind::kAddressComputation, thunk_info),
      embedded_thunk_(std::make_unique<SequentialThunk>(
          ThunkInfo(thunk_info.op), std::move(*embedded_thunk))),
      embedded_thunk_operands_(std::move(operands)),
      embedded_thunk_results_(std::move(results)),
      operand_offset_buffer_indices_(std::move(operand_offset_buffer_indices)),
      operand_orig_shapes_(std::move(operand_orig_shapes)),
      operand_sliced_shapes_(std::move(operand_sliced_shapes)),
      result_offset_buffer_indices_(std::move(result_offset_buffer_indices)),
      result_orig_shapes_(std::move(result_orig_shapes)),
      result_sliced_shapes_(std::move(result_sliced_shapes)) {}

absl::Status AddressComputationThunk::Prepare(
    const PrepareParams& params, ResourceRequests& resource_requests) {
  auto num_operands = embedded_thunk_operands_.size();
  TF_RET_CHECK(num_operands == operand_offset_buffer_indices_.size());
  TF_RET_CHECK(num_operands == operand_orig_shapes_.size());
  TF_RET_CHECK(num_operands == operand_sliced_shapes_.size());
  for (unsigned i = 0; i < num_operands; ++i) {
    if (operand_sliced_shapes_[i].has_value()) {
      TF_RET_CHECK(embedded_thunk_operands_[i].has_value());
      TF_RET_CHECK(operand_offset_buffer_indices_[i].has_value());
      TF_RET_CHECK(operand_sliced_shapes_[i]->IsArray());
      TF_RET_CHECK(operand_orig_shapes_[i].has_value() &&
                   operand_orig_shapes_[i]->IsArray());
    }
  }

  auto num_results = embedded_thunk_results_.size();
  TF_RET_CHECK(num_results == result_offset_buffer_indices_.size());
  TF_RET_CHECK(num_results == result_orig_shapes_.size());
  TF_RET_CHECK(num_results == result_sliced_shapes_.size());
  for (unsigned i = 0; i < num_results; ++i) {
    if (result_sliced_shapes_[i].has_value()) {
      TF_RET_CHECK(embedded_thunk_results_[i].has_value());
      TF_RET_CHECK(result_offset_buffer_indices_[i].has_value());
      TF_RET_CHECK(result_sliced_shapes_[i]->IsArray());
      TF_RET_CHECK(result_orig_shapes_[i].has_value() &&
                   result_orig_shapes_[i]->IsArray());
    }
  }

  TF_RETURN_IF_ERROR(embedded_thunk_->Prepare(params, resource_requests));
  return absl::OkStatus();
}

absl::Status AddressComputationThunk::Initialize(
    const InitializeParams& params) {
  TF_RETURN_IF_ERROR(embedded_thunk_->Initialize(params));

  unsigned operand_offset_count = 0;
  for (auto maybe_shape : operand_sliced_shapes_) {
    operand_offset_count +=
        (maybe_shape == std::nullopt) ? 1 : maybe_shape->rank();
  }

  {
    absl::MutexLock lock(&mutex_);
    if (auto it = operand_offsets_.find(params.executor);
        it == operand_offsets_.end()) {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<se::MemoryAllocation> allocation,
                          params.executor->HostMemoryAllocate(
                              operand_offset_count * sizeof(int64_t)));
      operand_offsets_.emplace(params.executor, std::move(allocation));
    }
  }

  unsigned result_offset_count = 0;
  for (auto maybe_shape : result_sliced_shapes_) {
    result_offset_count +=
        (maybe_shape == std::nullopt) ? 1 : maybe_shape->rank();
  }

  {
    absl::MutexLock lock(&mutex_);
    if (auto it = result_offsets_.find(params.executor);
        it == result_offsets_.end()) {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<se::MemoryAllocation> allocation,
                          params.executor->HostMemoryAllocate(
                              result_offset_count * sizeof(int64_t)));
      result_offsets_.emplace(params.executor, std::move(allocation));
    }
  }

  return absl::OkStatus();
}

absl::Status AddressComputationThunk::ExecuteOnStream(
    const ExecuteParams& params) {
  auto& stream = *params.stream;
  std::vector<se::DeviceMemoryBase> new_buffers;
  const BufferAllocations& orig_allocations = *params.buffer_allocations;

  // Get memory allocation for copying operand offsets from device.
  int64_t* operand_offsets_base = [&] {
    absl::MutexLock lock(&mutex_);
    return reinterpret_cast<int64_t*>(
        operand_offsets_.at(stream.parent())->opaque());
  }();

  for (unsigned i = 0; i < operand_offset_buffer_indices_.size(); ++i) {
    if (embedded_thunk_operands_[i] == std::nullopt) {
      new_buffers.push_back(se::DeviceMemoryBase());
      continue;
    }

    se::DeviceMemoryBase orig_operand =
        orig_allocations.GetDeviceAddress(*embedded_thunk_operands_[i]);
    if (operand_offset_buffer_indices_[i] == std::nullopt) {
      new_buffers.push_back(orig_operand);
      continue;
    }

    se::DeviceMemoryBase offset_src =
        orig_allocations.GetDeviceAddress(*operand_offset_buffer_indices_[i]);

    // Copy the ith offset from device to host.
    const Shape& src_shape = *operand_orig_shapes_[i];
    const Shape& dst_shape = *operand_sliced_shapes_[i];
    int64_t* offset_dst = &operand_offsets_base[i];
    TF_RETURN_IF_ERROR(stream.Memcpy(offset_dst, offset_src,
                                     dst_shape.rank() * sizeof(int64_t)));

    if (absl::Status blocked = stream.BlockHostUntilDone(); !blocked.ok()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to retrieve all slice offset values on stream %p: %s",
          &stream, blocked.message()));
    }

    // Compute new slice. No need to copy the content to new buffers as we can
    // reuse the original buffers since slices are contiguous.
    TF_RET_CHECK(IsContiguousSlice(src_shape, dst_shape));

    int64_t new_size = ShapeUtil::ByteSizeOf(dst_shape);
    BufferAllocation::Slice orig_slice = *embedded_thunk_operands_[i];

    int64_t new_offset = orig_slice.offset();
    std::vector<int64_t> slice_starts(offset_dst,
                                      offset_dst + dst_shape.rank());
    for (auto [start, stride] :
         llvm::zip(slice_starts, *ShapeUtil::ByteStrides(src_shape))) {
      new_offset += start * stride;
    }

    new_buffers.push_back(orig_operand.GetByteSlice(new_offset, new_size));
  }

  // Get memory allocation for copying result offsets from device.
  int64_t* result_offsets_base = [&] {
    absl::MutexLock lock(&mutex_);
    return reinterpret_cast<int64_t*>(
        result_offsets_.at(stream.parent())->opaque());
  }();

  for (unsigned i = 0; i < result_offset_buffer_indices_.size(); ++i) {
    if (embedded_thunk_results_[i] == std::nullopt) {
      new_buffers.push_back(se::DeviceMemoryBase());
      continue;
    }

    se::DeviceMemoryBase orig_result =
        orig_allocations.GetDeviceAddress(*embedded_thunk_results_[i]);
    if (result_offset_buffer_indices_[i] == std::nullopt) {
      new_buffers.push_back(orig_result);
      continue;
    }

    se::DeviceMemoryBase offset_src =
        orig_allocations.GetDeviceAddress(*result_offset_buffer_indices_[i]);

    // Copy the ith offset from device to host.
    const Shape& src_shape = *result_orig_shapes_[i];
    const Shape& dst_shape = *result_sliced_shapes_[i];
    int64_t* offset_dst = &result_offsets_base[i];
    TF_RETURN_IF_ERROR(stream.Memcpy(offset_dst, offset_src,
                                     dst_shape.rank() * sizeof(int64_t)));

    if (absl::Status blocked = stream.BlockHostUntilDone(); !blocked.ok()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to retrieve all slice offset values on stream %p: %s",
          &stream, blocked.message()));
    }

    // Compute new slice. No need to copy the content to new buffers as we can
    // reuse the original buffers since slices are contiguous.
    TF_RET_CHECK(IsContiguousSlice(src_shape, dst_shape));

    int64_t new_size = ShapeUtil::ByteSizeOf(dst_shape);
    BufferAllocation::Slice orig_slice = *embedded_thunk_results_[i];

    int64_t new_offset = orig_slice.offset();
    std::vector<int64_t> slice_starts(offset_dst,
                                      offset_dst + dst_shape.rank());
    for (auto [start, stride] :
         llvm::zip(slice_starts, *ShapeUtil::ByteStrides(src_shape))) {
      new_offset += start * stride;
    }

    new_buffers.push_back(orig_result.GetByteSlice(new_offset, new_size));
  }

  // Safe to create a local BufferAllocations here since buffers are only slices
  // of bigger ones allocated elsewhere.
  BufferAllocations new_allocations(new_buffers,
                                    orig_allocations.device_ordinal(),
                                    orig_allocations.memory_allocator(),
                                    orig_allocations.external_allocations());

  Thunk::ExecuteParams new_params =
      Thunk::ExecuteParams::CloneWithNewAllocations(params, new_allocations);

  // Execute the underlying custom call thunk with the new buffers.
  TF_RETURN_IF_ERROR(embedded_thunk_->ExecuteOnStream(new_params));

  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
