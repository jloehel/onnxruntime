// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "onnxruntime_pybind_mlvalue.h"
#include "python/onnxruntime_pybind_state_common.h"
#include "pybind11/numpy.h"

#define NO_IMPORT_ARRAY
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL onnxruntime_python_ARRAY_API
#include <numpy/arrayobject.h>

#include "core/framework/tensor_shape.h"
#include "core/framework/tensor.h"
#include "core/framework/sparse_tensor.h"
#include "core/framework/sparse_cooformat_rep.h"
#include "core/framework/sparse_csrcformat_rep.h"
#include "core/framework/allocator.h"
#include "core/framework/data_types.h"

#include "core/framework/data_types_internal.h"
#include "core/providers/get_execution_providers.h"
#include "core/framework/kernel_registry.h"
#include "core/framework/provider_bridge_ort.h"
#include "core/framework/provider_options_utils.h"

namespace onnxruntime {
namespace python {

namespace py = pybind11;
using namespace onnxruntime::logging;

void addSparseTensorMethods(pybind11::module& m) {
  py::class_<PySparseTensor> sparse_tensor_binding(m, "SparseTensor");
  sparse_tensor_binding
      // Factor method to create a COO Sparse Tensor from numpy arrays acting as backing storage.
      // All data is assumed to be on cpu. Use numpy.ascontiguousarray() to obtain contiguous array of values and indices
      // if necessary
      // py_dense_shape - numpy dense shape of the sparse tensor
      // py_values - contiguous and homogeneous numpy array of values
      // py_indices - contiguous numpy array of int64_t indices
      .def_static("sparse_coo_from_numpy", [](const py::array_t<int64_t>& py_dense_shape,
                                              const py::array& py_values, 
                                              const py::array_t<int64_t>& py_indices) -> std::unique_ptr<PySparseTensor> {
        if (!PyArray_ISCONTIGUOUS(reinterpret_cast<PyArrayObject*>(py_values.ptr()))) {
          throw std::runtime_error("Require contiguous numpy array of values");
        }

        if (!PyArray_ISCONTIGUOUS(reinterpret_cast<PyArrayObject*>(py_indices.ptr()))) {
          throw std::runtime_error("Require contiguous numpy array of indices");
        }

        if (1 != py_values.ndim()) {
          ORT_THROW("Expecting values 1-D numpy values array for COO format. Got dims: ", py_values.ndim());
        }

        const auto num_ind_dims = py_indices.ndim();
        TensorShape dense_shape(py_dense_shape.data(), py_dense_shape.size());
        auto values_type = GetNumpyArrayType(py_values);
        auto ml_type = NumpyTypeToOnnxRuntimeType(values_type);

        std::unique_ptr<PySparseTensor> result;
        if (IsNumericNumpyType(values_type) && py_values.owndata() && py_indices.owndata()) {
          // go ahead and create references to make sure storage does not disappear
          std::vector<py::object> reference_holders = {py_values, py_indices};
          // Make sure that py_values is contiguous
          auto sparse_tensor = std::make_unique<SparseTensor>(ml_type, dense_shape,
                                                              py_values.size(),
                                                              const_cast<void*>(py_values.data()), 
                                                              OrtMemoryInfo());
          SparseCooFormatRep* rep;
          ORT_THROW_IF_ERROR(sparse_tensor->RepBuilder<SparseCooBuilder>()
                                 .GetOrCreate(GetShape(py_indices), const_cast<int64_t*>(py_indices.data()), rep));
          result = std::make_unique<PySparseTensor>(std::move(sparse_tensor), std::move(reference_holders));
        } else if (values_type == NPY_UNICODE || values_type == NPY_STRING || values_type == NPY_VOID) {
          const bool linear_index = num_ind_dims == 1;
          auto sparse_tensor = std::make_unique<SparseTensor>(ml_type, dense_shape, py_values.size(), GetAllocator());
          SparseCooFormatRep* rep;
          ORT_THROW_IF_ERROR(sparse_tensor->RepBuilder<SparseCooBuilder>().GetOrCreate(linear_index, rep));
          CopyDataToTensor(py_values, values_type, sparse_tensor->MutableValues());
          CopyDataToTensor(py_indices, GetNumpyArrayType(py_indices), rep->MutableIndices());
          result = std::make_unique<PySparseTensor>(std::move(sparse_tensor));
        } else {
          ORT_THROW("Unsupported values data type: ", values_type);
        }

        return result;
      })
      .def_static("sparse_csr_from_numpy", [](const py::array_t<int64_t>& py_dense_shape, 
                                       const py::array& py_values,
                                       const py::array_t<int64_t>& py_inner_indices,
                                       const py::array_t<int64_t>& py_outer_indices) -> std::unique_ptr<PySparseTensor> {

        if (!PyArray_ISCONTIGUOUS(reinterpret_cast<PyArrayObject*>(py_values.ptr()))) {
          throw std::runtime_error("Require contiguous numpy array of values");
        }

        if (!PyArray_ISCONTIGUOUS(reinterpret_cast<PyArrayObject*>(py_inner_indices.ptr()))) {
          throw std::runtime_error("Require contiguous numpy array of indices");
        }

        if (!PyArray_ISCONTIGUOUS(reinterpret_cast<PyArrayObject*>(py_outer_indices.ptr()))) {
          throw std::runtime_error("Require contiguous numpy array of indices");
        }

        if (1 != py_values.ndim() || 1 != py_inner_indices.ndim() || 1 != py_outer_indices.ndim()) {
          ORT_THROW("Expecting all data to be 1-D numpy arrays for CSR format.");
        }

        TensorShape dense_shape(py_dense_shape.data(), py_dense_shape.size());
        auto values_type = GetNumpyArrayType(py_values);
        auto ml_type = NumpyTypeToOnnxRuntimeType(values_type);

                std::unique_ptr<PySparseTensor> result;
        if (IsNumericNumpyType(values_type) && py_values.owndata() &&
          py_inner_indices.owndata() && py_outer_indices.owndata()) {
          // go ahead and create references to make sure storage does not disappear
          std::vector<py::object> reference_holders = {py_values, py_inner_indices, py_outer_indices};
          // Make sure that py_values is contiguous
          auto sparse_tensor = std::make_unique<SparseTensor>(ml_type, dense_shape,
                                                              py_values.size(),
                                                              const_cast<void*>(py_values.data()),
                                                              OrtMemoryInfo());
          SparseCsrcFormatRep* rep;
          ORT_THROW_IF_ERROR(sparse_tensor->RepBuilder<SparseCsrcBuilder>()
                                                      .GetOrCreate(SparseCsrcFormatRep::kRowMajor,
                                                          GetShape(py_inner_indices), GetShape(py_outer_indices),
                                                          const_cast<int64_t*>(py_inner_indices.data()),
                                                          const_cast<int64_t*>(py_outer_indices.data()), rep));
          result = std::make_unique<PySparseTensor>(std::move(sparse_tensor), std::move(reference_holders));
        } else if (values_type == NPY_UNICODE || values_type == NPY_STRING || values_type == NPY_VOID) {
          auto sparse_tensor = std::make_unique<SparseTensor>(ml_type, dense_shape, py_values.size(), GetAllocator());
          SparseCsrcFormatRep* rep;
          ORT_THROW_IF_ERROR(sparse_tensor->RepBuilder<SparseCsrcBuilder>()
                                 .GetOrCreate(SparseCsrcFormatRep::kRowMajor,
                                              GetShape(py_inner_indices), GetShape(py_outer_indices),
                                              rep));
          CopyDataToTensor(py_values, values_type, sparse_tensor->MutableValues());
          CopyDataToTensor(py_inner_indices, GetNumpyArrayType(py_inner_indices), rep->MutableInner());
          CopyDataToTensor(py_outer_indices, GetNumpyArrayType(py_outer_indices), rep->MutableOuter());
          result = std::make_unique<PySparseTensor>(std::move(sparse_tensor));
        } else {
          ORT_THROW("Unsupported values data type: ", values_type);
        }

        return result;
       })
      .def("shape", [](const PySparseTensor* py_tensor) -> py::array_t<int64_t> {
        const SparseTensor& st = py_tensor->Instance();
        const auto& dims = st.Shape().GetDims();
        // We create a copy of dimensions, it is small
        py::array_t<int64_t> result({static_cast<int64_t>(dims.size())});
        auto span = gsl::make_span(result.mutable_data(), result.size());
        std::copy(dims.cbegin(), dims.cend(), span.begin());
        return result;
       });
}

}  // namespace python
}  // namespace onnxruntime