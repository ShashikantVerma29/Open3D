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
#include "open3d/core/Dispatch.h"
#include "open3d/core/Indexer.h"
#include "open3d/core/SizeVector.h"
#include "open3d/core/kernel/CumSum.h"
#include "open3d/core/kernel/ParallelUtil.h"
#include "open3d/utility/Console.h"

namespace open3d {
namespace core {
namespace kernel {

Tensor CumSumCPU(const Tensor& src, const SizeVector& dims) {
    // Check dims.
    if (dims.size() != 1) {
        utility::LogError("CumSum can only have 1 reduction dimension.");
    }
    int64_t dim = dims[0];
    if (dim < 0 || dim >= src.NumDims()) {
        utility::LogError("Dimension out of range.");
    }

    // Destination Tensor.
    Tensor dst(src.GetShape(), src.GetDtype(), src.GetDevice());
    dst.CopyFrom(src);

    int64_t num_elements = src.GetShapeRef()[dim];

    Tensor dst_transpose = dst.Transpose(0, dim);
    for (int64_t i = 1; i < num_elements; ++i) {
        Tensor dst_slice = dst_transpose[i];
        Tensor prev_slice = dst_transpose[i - 1];
        dst_slice.Add_(prev_slice);
    }
    return dst;
}

}  // namespace kernel
}  // namespace core
}  // namespace open3d