// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "Open3D/Container/Tensor.h"

#include <sstream>

#include "Open3D/Container/AdvancedIndexing.h"
#include "Open3D/Container/Blob.h"
#include "Open3D/Container/Broadcast.h"
#include "Open3D/Container/Device.h"
#include "Open3D/Container/Dispatch.h"
#include "Open3D/Container/Dtype.h"
#include "Open3D/Container/Kernel/Kernel.h"
#include "Open3D/Container/SizeVector.h"
#include "Open3D/Utility/Console.h"

namespace open3d {

/// Tensor assignment lvalue = lvalue, e.g. `tensor_a = tensor_b`
Tensor& Tensor::operator=(const Tensor& other) & {
    shape_ = other.shape_;
    strides_ = other.strides_;
    dtype_ = other.dtype_;
    device_ = other.device_;
    blob_ = other.blob_;
    data_ptr_ = other.data_ptr_;
    return *this;
}

/// Tensor assignment lvalue = rvalue, e.g. `tensor_a = tensor_b[0]`
Tensor& Tensor::operator=(Tensor&& other) & {
    shape_ = other.shape_;
    strides_ = other.strides_;
    dtype_ = other.dtype_;
    device_ = other.device_;
    blob_ = other.blob_;
    data_ptr_ = other.data_ptr_;
    return *this;
}

/// Tensor assignment rvalue = lvalue, e.g. `tensor_a[0] = tensor_b`
Tensor& Tensor::operator=(const Tensor& other) && {
    kernel::Copy(other, *this);
    return *this;
}

/// Tensor assignment rvalue = rvalue, e.g. `tensor_a[0] = tensor_b[0]`
Tensor& Tensor::operator=(Tensor&& other) && {
    kernel::Copy(other, *this);
    return *this;
}

/// Assign (copy) values from another Tensor, shape, dtype, device may change.
void Tensor::Assign(const Tensor& other) {
    shape_ = other.shape_;
    strides_ = DefaultStrides(shape_);
    dtype_ = other.dtype_;
    device_ = other.device_;
    blob_ = std::make_shared<Blob>(
            shape_.NumElements() * DtypeUtil::ByteSize(dtype_), device_);
    data_ptr_ = blob_->v_;
    kernel::Copy(other, *this);
}

/// Broadcast Tensor to a new broadcastable shape
Tensor Tensor::Broadcast(const SizeVector& dst_shape) const {
    if (!CanBeBrocastedToShape(shape_, dst_shape)) {
        utility::LogError("Cannot broadcast shape {} to shape {}.",
                          shape_.ToString(), dst_shape);
    }
    Tensor dst_tensor(dst_shape, dtype_, device_);
    dst_tensor.AsRvalue() = *this;
    return dst_tensor;
}

Tensor Tensor::Expand(const SizeVector& dst_shape) const {
    if (!CanBeBrocastedToShape(shape_, dst_shape)) {
        utility::LogError("Cannot expand shape {} to shape {}.",
                          shape_.ToString(), dst_shape);
    }
    int64_t src_ndims = NumDims();
    int64_t dst_ndims = dst_shape.size();
    int64_t omitted_ndims = dst_ndims - src_ndims;

    // Fill 1 in shape for omitted dimensions in front.
    // Noe that unexpanded_new_shape is not the expanded shape. The expanded
    // shape is the dst_shape.
    SizeVector unexpanded_new_shape(dst_ndims, 1);
    for (int64_t i = 0; i < src_ndims; ++i) {
        unexpanded_new_shape[i + omitted_ndims] = shape_[i];
    }

    // Fill 0 in strides for omitted dimensions in front.
    SizeVector new_strides(dst_ndims, 0);
    for (int64_t i = 0; i < src_ndims; ++i) {
        new_strides[i + omitted_ndims] = strides_[i];
    }

    // Set stride to 0 if the dimension is expanded.
    for (int64_t i = 0; i < dst_ndims; ++i) {
        if (unexpanded_new_shape[i] == 1 && dst_shape[i] != 1) {
            new_strides[i] = 0;
        }
    }

    return AsStrided(dst_shape, new_strides);
}

Tensor Tensor::Reshape(const SizeVector& dst_shape) const {
    SizeVector inferred_dst_shape = InferShape(dst_shape, NumElements());
    bool can_restride;
    SizeVector new_strides;
    std::tie(can_restride, new_strides) =
            ComputeNewStrides(shape_, strides_, inferred_dst_shape);
    if (can_restride) {
        return AsStrided(inferred_dst_shape, new_strides);
    } else {
        return Contiguous().View(inferred_dst_shape);
    }
}

Tensor Tensor::View(const SizeVector& dst_shape) const {
    SizeVector inferred_dst_shape = InferShape(dst_shape, NumElements());
    bool can_restride;
    SizeVector new_strides;
    std::tie(can_restride, new_strides) =
            ComputeNewStrides(shape_, strides_, inferred_dst_shape);
    if (can_restride) {
        return AsStrided(inferred_dst_shape, new_strides);
    } else {
        utility::LogError(
                "View shape {} is not compatible with Tensor's size {} and "
                "sride {}, at least one dimension spacs across two contiguous "
                "subspaces. Use Reshape() instead.",
                dst_shape, shape_, strides_);
    }
}

Tensor Tensor::Copy(const Device& device) const {
    Tensor dst_tensor(shape_, dtype_, device);
    kernel::Copy(*this, dst_tensor);
    return dst_tensor;
}

void Tensor::CopyFrom(const Tensor& other) { AsRvalue() = other; }

Tensor Tensor::Clone(const Device& device) const {
    auto new_blob = std::make_shared<Blob>(blob_->byte_size_, device);
    MemoryManager::MemcpyBlob(new_blob, blob_);
    int64_t data_offset =
            static_cast<char*>(data_ptr_) - static_cast<char*>(blob_->v_);
    void* new_data_ptr = static_cast<char*>(new_blob->v_) + data_offset;
    return Tensor(shape_, strides_, new_data_ptr, dtype_, device, new_blob);
}

Tensor Tensor::Contiguous() const {
    if (IsContiguous()) {
        // Returns a shallow copy of the current Tensor
        return Tensor(shape_, strides_, data_ptr_, dtype_, device_, blob_);
    } else {
        // Compact the tensor to contiguous on the same device
        return Copy(device_);
    }
}

SizeVector Tensor::DefaultStrides(const SizeVector& shape) {
    SizeVector strides(shape.size());
    int64_t stride_size = 1;
    for (int64_t i = shape.size(); i > 0; --i) {
        strides[i - 1] = stride_size;
        // Handles 0-sized dimensions
        stride_size *= std::max<int64_t>(shape[i - 1], 1);
    }
    return strides;
}

std::pair<bool, SizeVector> Tensor::ComputeNewStrides(
        const SizeVector& old_shape,
        const SizeVector& old_strides,
        const SizeVector& new_shape) {
    if (old_shape.empty()) {
        return std::make_pair(true, SizeVector(new_shape.size(), 1));
    }

    // NOTE: stride is arbitrary in the numel() == 0 case;
    // to match NumPy behavior we copy the strides if the size matches,
    // otherwise we use the stride as if it were computed via resize. This could
    // perhaps be combined with the below code, but the complexity didn't seem
    // worth it.
    int64_t numel = old_shape.NumElements();
    if (numel == 0 && old_shape == new_shape) {
        return std::make_pair(true, old_strides);
    }

    SizeVector new_strides(new_shape.size());
    if (numel == 0) {
        for (int64_t view_d = new_shape.size() - 1; view_d >= 0; view_d--) {
            if (view_d == (int64_t)(new_shape.size() - 1)) {
                new_strides[view_d] = 1;
            } else {
                new_strides[view_d] =
                        std::max<int64_t>(new_shape[view_d + 1], 1) *
                        new_strides[view_d + 1];
            }
        }
        return std::make_pair(true, new_strides);
    }

    int64_t view_d = new_shape.size() - 1;
    // stride for each subspace in the chunk
    int64_t chunk_base_stride = old_strides.back();
    // numel in current chunk
    int64_t tensor_numel = 1;
    int64_t view_numel = 1;
    for (int64_t tensor_d = old_shape.size() - 1; tensor_d >= 0; tensor_d--) {
        tensor_numel *= old_shape[tensor_d];
        // if end of tensor size chunk, check view
        if ((tensor_d == 0) ||
            (old_shape[tensor_d - 1] != 1 &&
             old_strides[tensor_d - 1] != tensor_numel * chunk_base_stride)) {
            while (view_d >= 0 &&
                   (view_numel < tensor_numel || new_shape[view_d] == 1)) {
                new_strides[view_d] = view_numel * chunk_base_stride;
                view_numel *= new_shape[view_d];
                view_d--;
            }
            if (view_numel != tensor_numel) {
                return std::make_pair(false, SizeVector());
            }
            if (tensor_d > 0) {
                chunk_base_stride = old_strides[tensor_d - 1];
                tensor_numel = 1;
                view_numel = 1;
            }
        }
    }
    if (view_d != -1) {
        return std::make_pair(false, SizeVector());
    }
    return std::make_pair(true, new_strides);
}

std::string Tensor::ToString(bool with_suffix,
                             const std::string& indent) const {
    std::ostringstream rc;

    if (device_.device_type_ == Device::DeviceType::CUDA || !IsContiguous()) {
        Tensor host_contiguous_tensor = Copy(Device("CPU:0"));
        rc << host_contiguous_tensor.ToString(false, "");
    } else {
        if (shape_.size() == 0) {
            rc << indent;
            rc << ScalarPtrToString(data_ptr_);
        } else if (shape_.size() == 1) {
            const char* ptr = static_cast<const char*>(data_ptr_);
            rc << "[";
            std::string delim = "";
            int64_t element_byte_size = DtypeUtil::ByteSize(dtype_);
            for (int64_t i = 0; i < shape_.NumElements(); ++i) {
                rc << delim << ScalarPtrToString(ptr);
                delim = " ";
                ptr += element_byte_size;
            }
            rc << "]";
        } else {
            rc << "[";
            std::string delim = "";
            std::string child_indent = "";
            for (int64_t i = 0; i < shape_[0]; ++i) {
                rc << delim << child_indent
                   << this->operator[](i).ToString(false, indent + " ");
                delim = ",\n";
                child_indent = indent + " ";
            }
            rc << "]";
        }
    }
    if (with_suffix) {
        rc << fmt::format("\nTensor[shape={}, stride={}, {}, {}, {}]",
                          shape_.ToString(), strides_.ToString(),
                          DtypeUtil::ToString(dtype_), device_.ToString(),
                          data_ptr_);
    }
    return rc.str();
}

std::string Tensor::ScalarPtrToString(const void* ptr) const {
    std::string str = "";
    DISPATCH_DTYPE_TO_TEMPLATE(dtype_, [&]() {
        str = fmt::format("{}", *static_cast<const scalar_t*>(ptr));
    });
    return str;
}

Tensor Tensor::operator[](int64_t i) const {
    if (shape_.size() == 0) {
        utility::LogError("Tensor has shape (), cannot be indexed.");
    }
    if (i < 0) {
        utility::LogError("Only non-ngegative index is supported, but {} < 0.",
                          i);
    }
    if (i >= shape_[0]) {
        utility::LogError("Index {} is out of bounds for axis of length {}.", i,
                          shape_[0]);
    }
    if (shape_.size() != strides_.size()) {
        utility::LogError(
                "Internal error, shape and strides dimension mismatch {} != "
                "{}",
                shape_.size(), strides_.size());
    }
    SizeVector new_shape(shape_.begin() + 1, shape_.end());
    SizeVector new_stride(strides_.begin() + 1, strides_.end());
    void* new_data_ptr = static_cast<char*>(data_ptr_) +
                         strides_[0] * DtypeUtil::ByteSize(dtype_) * i;
    return Tensor(new_shape, new_stride, new_data_ptr, dtype_, device_, blob_);
}

Tensor Tensor::Slice(int64_t dim,
                     int64_t start,
                     int64_t stop,
                     int64_t step) const {
    if (shape_.size() == 0) {
        utility::LogError("Slice cannot be applied to 0-dim Tensor");
    }
    if (dim < 0 || dim >= shape_.size()) {
        utility::LogError("Dim {} is out of bound for SizeVector of length {}",
                          dim, shape_.size());
    }
    // TODO: support negative step sizes
    if (step == 0) {
        utility::LogError("Step size cannot be 0");
    }
    // TODO: support wrap-around start/stop index
    if (start < 0 || start >= shape_[dim]) {
        utility::LogError("Index {} is out of bounds for axis of length {}.",
                          start, shape_[dim]);
    }
    // The stop index is non-inclusive
    if (stop < 0 || stop > shape_[dim]) {
        utility::LogError("Index {} is out of bounds for axis of length {}.",
                          stop, shape_[dim]);
    }
    if (stop < start) {
        stop = start;
    }

    void* new_data_ptr = static_cast<char*>(data_ptr_) +
                         start * strides_[dim] * DtypeUtil::ByteSize(dtype_);
    SizeVector new_shape = shape_;
    SizeVector new_strides = strides_;
    new_shape[dim] = (stop - start + step - 1) / step;
    new_strides[dim] = strides_[dim] * step;
    return Tensor(new_shape, new_strides, new_data_ptr, dtype_, device_, blob_);
}

Tensor Tensor::IndexGet(const std::vector<Tensor>& index_tensors) const {
    AdvancedIndexPreprocessor aip(*this, index_tensors);
    Tensor dst = Tensor(aip.GetOutputShape(), dtype_, device_);
    kernel::IndexGet(aip.GetTensor(), dst, aip.GetIndexTensors(),
                     aip.GetIndexedShape(), aip.GetIndexedStrides());

    return dst;
}

void Tensor::IndexSet(const std::vector<Tensor>& index_tensors,
                      const Tensor& src_tensor) {
    AdvancedIndexPreprocessor aip(*this, index_tensors);
    Tensor pre_processed_dst = aip.GetTensor();
    kernel::IndexSet(src_tensor, pre_processed_dst, aip.GetIndexTensors(),
                     aip.GetIndexedShape(), aip.GetIndexedStrides());
}

Tensor Tensor::Permute(const SizeVector& dims) const {
    // Check dimension size
    if (static_cast<int64_t>(dims.size()) != NumDims()) {
        utility::LogError(
                "Tensor has {} dimensions, but permuntation have {} "
                "dimensions.",
                NumDims(), dims.size());
    }
    int64_t n_dims = NumDims();

    // Check dims are permuntation of [0, 1, 2, ..., n-1]
    std::vector<bool> seen_dims(n_dims, false);
    for (const int64_t& dim : dims) {
        seen_dims[WrapDim(dim, n_dims)] = true;
    }
    if (!std::all_of(seen_dims.begin(), seen_dims.end(),
                     [](bool seen) { return seen; })) {
        utility::LogError("Permute dims must be a permuntation from 0 to {}",
                          dims.size() - 1);
    }

    // Map to new shape and strides
    const SizeVector& old_shape = shape_;
    const SizeVector& old_stides = strides_;
    SizeVector new_shape(n_dims);
    SizeVector new_strides(n_dims);
    for (int64_t i = 0; i < n_dims; ++i) {
        int64_t old_dim = WrapDim(dims[i], n_dims);
        new_shape[i] = old_shape[old_dim];
        new_strides[i] = old_stides[old_dim];
    }

    return AsStrided(new_shape, new_strides);
}

Tensor Tensor::AsStrided(const SizeVector& new_shape,
                         const SizeVector& new_strides) const {
    Tensor result(new_shape, new_strides, const_cast<void*>(data_ptr_), dtype_,
                  device_, blob_);
    return result;
}

Tensor Tensor::Transpose(int64_t dim0, int64_t dim1) const {
    int64_t n_dims = NumDims();
    dim0 = WrapDim(dim0, n_dims);
    dim1 = WrapDim(dim1, n_dims);
    SizeVector dims(n_dims);
    std::iota(dims.begin(), dims.end(), 0);
    dims[dim0] = dim1;
    dims[dim1] = dim0;
    return Permute(dims);
}

Tensor Tensor::T() const {
    int64_t n_dims = NumDims();
    if (n_dims <= 1) {
        return *this;
    } else if (n_dims == 2) {
        return Transpose(0, 1);
    } else {
        utility::LogError(
                "Tensor::T() expects a Tensor with <= 2 dimensions, but the "
                "Tensor as {} dimensions.");
    }
}

}  // namespace open3d