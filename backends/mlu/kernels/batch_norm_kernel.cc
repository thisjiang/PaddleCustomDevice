// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/mlu_baseop.h"
#include "kernels/funcs/mlu_funcs.h"

namespace custom_kernel {

template <typename T, typename Context>
void BatchNormKernel(const Context& dev_ctx,
                     const phi::DenseTensor& x,
                     const phi::DenseTensor& running_mean,
                     const phi::DenseTensor& running_var,
                     const phi::DenseTensor& scale,
                     const phi::DenseTensor& bias,
                      bool is_test,
                     float momentum,
                     float epsilon,
                     const std::string& data_layout_str,
                     bool use_global_stats,
                     bool trainable_stats,
                     phi::DenseTensor* y,
                     phi::DenseTensor* mean_out,
                     phi::DenseTensor* variance_out,
                     phi::DenseTensor* saved_mean,
                     phi::DenseTensor* saved_variance,
                     phi::DenseTensor* reserve_space) {
  bool test_mode = is_test && (!trainable_stats);
  bool global_stats = test_mode || use_global_stats;

  DataLayout data_layout = StringToDataLayout(data_layout_str);

  const auto& x_dims = x.dims();
  PADDLE_ENFORCE_GE(
      x_dims.size(),
      2,
      phi::errors::InvalidArgument(
          "The size of input X's dimensions should be larger than 1."
          "But received: the size of input X's dimensions is [%d]",
          x_dims.size()));
  PADDLE_ENFORCE_LE(
      x_dims.size(),
      5,
      phi::errors::InvalidArgument(
          "The size of input X's dimensions should be less than 6."
          "But received: the size of input X's dimensions is [%d]",
          x_dims.size()));
  const int N = x_dims[0];
  const int C = (data_layout == DataLayout::kNCHW ? x_dims[1]
                                                  : x_dims[x_dims.size() - 1]);
  const int sample_size = x.numel() / N / C;

  // alloc memory
  dev_ctx.template Alloc<T>(y);

  using MPDType = typename MPTypeTrait<T>::Type;
  dev_ctx.template Alloc<MPDType>(mean_out);
  dev_ctx.template Alloc<MPDType>(variance_out);
  dev_ctx.template Alloc<MPDType>(saved_mean);
  dev_ctx.template Alloc<MPDType>(saved_variance);

  Tensor transformed_x;
  Tensor transformed_y;
  const int transformed_dim_size = 4;
  const int transformed_shape[transformed_dim_size] = {N, sample_size, 1, C};
  MLUCnnlTensorDesc transformed_desc(transformed_dim_size,
                                     transformed_shape,
                                     ToCnnlDataType<T>(),
                                     CNNL_LAYOUT_NHWC);
  MLUCnnlTensorDesc others_input_desc(scale);
  // input dimension is 2 and the format is NCHW. The input can be regarded as
  // NHWC format. Don't need to transpose.
  bool need_transpose =
      (data_layout == DataLayout::kNCHW && x_dims.size() != 2);
  if (need_transpose) {
    transformed_x.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_x);
    transformed_y.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_y);

    const std::vector<int> perm = {0, 2, 3, 1};
    const int x_reshaped[] = {N, C, sample_size, 1};
    MLUCnnlTensorDesc x_reshaped_desc(
        transformed_dim_size, x_reshaped, ToCnnlDataType<T>());
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_dim_size,
                       x_reshaped_desc.get(),
                       GetBasePtr(&x),
                       transformed_desc.get(),
                       GetBasePtr(&transformed_x));
  } else {
    transformed_x = x;
    transformed_y = *y;
  }

  MLUCnnl::FusedBatchNorm(dev_ctx,
                          !global_stats,
                          transformed_desc.get(),
                          GetBasePtr(&transformed_x),
                          others_input_desc.get(),
                          GetBasePtr(&scale),
                          GetBasePtr(&bias),
                          GetBasePtr(&running_mean),
                          GetBasePtr(&running_var),
                          epsilon,
                          momentum,
                          transformed_desc.get(),
                          GetBasePtr(&transformed_y),
                          GetBasePtr(mean_out),
                          GetBasePtr(variance_out),
                          GetBasePtr(saved_mean),
                          GetBasePtr(saved_variance));

  if (need_transpose) {
    const int y_reshaped[] = {N, C, sample_size, 1};
    MLUCnnlTensorDesc y_reshaped_desc(
        transformed_dim_size, y_reshaped, ToCnnlDataType<T>());
    const std::vector<int> perm = {0, 3, 1, 2};
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_y.dims().size(),
                       transformed_desc.get(),
                       GetBasePtr(&transformed_y),
                       y_reshaped_desc.get(),
                       GetBasePtr(y));
  }
}

template <typename T, typename Context>
void BatchNormGradKernel(
    const Context& dev_ctx,
    const phi::DenseTensor& x,
    const phi::DenseTensor& scale,
    const phi::DenseTensor& bias,
    const paddle::optional<phi::DenseTensor>& mean,
    const paddle::optional<phi::DenseTensor>& variance,
    const phi::DenseTensor& saved_mean,
    const phi::DenseTensor& saved_inv_variance,
    const paddle::optional<phi::DenseTensor>& reserve_space,
    const phi::DenseTensor& d_y,
    float momentum,
    float epsilon,
    const std::string& data_layout_str,
    bool is_test,
    bool use_global_stats,
    bool trainable_statistics,
    phi::DenseTensor* d_x,
    phi::DenseTensor* d_scale,
    phi::DenseTensor* d_bias) {
  DataLayout data_layout = StringToDataLayout(data_layout_str);

  Tensor d_x_tmp;
  if (d_x == nullptr) {
    d_x = &d_x_tmp;
    d_x->Resize(x.dims());
  }
  Tensor scale_grad_tmp;
  if (d_scale == nullptr) {
    d_scale = &scale_grad_tmp;
    d_scale->Resize(scale.dims());
  }
  Tensor bias_grad_tmp;
  if (d_bias == nullptr) {
    d_bias = &bias_grad_tmp;
    d_bias->Resize(bias.dims());
  }

  dev_ctx.template Alloc<T>(d_x);
  using MPDType = typename MPTypeTrait<T>::Type;
  dev_ctx.template Alloc<MPDType>(d_scale);
  dev_ctx.template Alloc<MPDType>(d_bias);

  use_global_stats = is_test || use_global_stats;

  const auto& x_dims = x.dims();
  PADDLE_ENFORCE_GE(
      x_dims.size(),
      2,
      phi::errors::InvalidArgument(
          "The size of input X's dimensions should be larger than 1."
          "But received: the size of input X's dimensions is [%d]",
          x_dims.size()));
  PADDLE_ENFORCE_LE(
      x_dims.size(),
      5,
      phi::errors::InvalidArgument(
          "The size of input X's dimensions should be less than 6."
          "But received: the size of input X's dimensions is [%d]",
          x_dims.size()));
  const int N = x_dims[0];
  const int C = (data_layout == DataLayout::kNCHW ? x_dims[1]
                                                  : x_dims[x_dims.size() - 1]);
  const int sample_size = x.numel() / N / C;

  Tensor transformed_d_y;
  Tensor transformed_x;
  Tensor transformed_d_x;
  const int transformed_dim_size = 4;
  const int transformed_shape[transformed_dim_size] = {N, sample_size, 1, C};

  MLUCnnlTensorDesc transformed_desc(transformed_dim_size,
                                     transformed_shape,
                                     ToCnnlDataType<T>(),
                                     CNNL_LAYOUT_NHWC);
  MLUCnnlTensorDesc others_input_desc(scale);

  bool need_transpose =
      (data_layout == DataLayout::kNCHW && x_dims.size() != 2);
  if (need_transpose) {
    transformed_d_y.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_d_y);
    transformed_x.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_x);
    transformed_d_x.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_d_x);

    const int org_reshaped[] = {N, C, sample_size, 1};
    MLUCnnlTensorDesc org_reshaped_desc(
        transformed_dim_size, org_reshaped, ToCnnlDataType<T>());
    const std::vector<int> perm = {0, 2, 3, 1};
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_dim_size,
                       org_reshaped_desc.get(),
                       GetBasePtr(&d_y),
                       transformed_desc.get(),
                       GetBasePtr(&transformed_d_y));
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_dim_size,
                       org_reshaped_desc.get(),
                       GetBasePtr(&x),
                       transformed_desc.get(),
                       GetBasePtr(&transformed_x));
  } else {
    transformed_d_y = d_y;
    transformed_x = x;
    transformed_d_x = *d_x;
  }

  if (use_global_stats) {
    const auto* running_mean = mean.get_ptr();
    const auto* running_variance = variance.get_ptr();
    MLUCnnl::FusedBatchNormGrad(dev_ctx,
                                false /*is_training*/,
                                transformed_desc.get(),
                                GetBasePtr(&transformed_d_y),
                                transformed_desc.get(),
                                GetBasePtr(&transformed_x),
                                others_input_desc.get(),
                                GetBasePtr(&scale),
                                GetBasePtr(running_mean),
                                GetBasePtr(running_variance),
                                epsilon,
                                transformed_desc.get(),
                                GetBasePtr(&transformed_d_x),
                                GetBasePtr(d_scale),
                                GetBasePtr(d_bias));
  } else {
    MLUCnnl::FusedBatchNormGrad(dev_ctx,
                                true /*is_training*/,
                                transformed_desc.get(),
                                GetBasePtr(&transformed_d_y),
                                transformed_desc.get(),
                                GetBasePtr(&transformed_x),
                                others_input_desc.get(),
                                GetBasePtr(&scale),
                                GetBasePtr(&saved_mean),
                                GetBasePtr(&saved_inv_variance),
                                epsilon,
                                transformed_desc.get(),
                                GetBasePtr(&transformed_d_x),
                                GetBasePtr(d_scale),
                                GetBasePtr(d_bias));
  }

  if (need_transpose) {
    const int d_x_reshaped[] = {N, C, sample_size, 1};
    MLUCnnlTensorDesc d_x_reshaped_desc(
        transformed_dim_size, d_x_reshaped, ToCnnlDataType<T>());
    const std::vector<int> perm = {0, 3, 1, 2};
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_dim_size,
                       transformed_desc.get(),
                       GetBasePtr(&transformed_d_x),
                       d_x_reshaped_desc.get(),
                       GetBasePtr(d_x));
  }
}

template <typename T, typename Context>
void BatchNormInferKernel(const Context& dev_ctx,
                          const phi::DenseTensor& x,
                          const phi::DenseTensor& mean,
                          const phi::DenseTensor& variance,
                          const phi::DenseTensor& scale,
                          const phi::DenseTensor& bias,
                          float momentum,
                          float epsilon,
                          const std::string& data_layout_str,
                          phi::DenseTensor* y,
                          phi::DenseTensor* mean_out,
                          phi::DenseTensor* variance_out) {
  DataLayout data_layout = StringToDataLayout(data_layout_str);

  const auto& x_dims = x.dims();
  PADDLE_ENFORCE_GE(
      x_dims.size(),
      2,
      phi::errors::InvalidArgument(
          "The size of input X's dimensions should be larger than 1."
          "But received: the size of input X's dimensions is [%d]",
          x_dims.size()));
  PADDLE_ENFORCE_LE(
      x_dims.size(),
      5,
      phi::errors::InvalidArgument(
          "The size of input X's dimensions should be less than 6."
          "But received: the size of input X's dimensions is [%d]",
          x_dims.size()));
  const int N = x_dims[0];
  const int C = (data_layout == DataLayout::kNCHW ? x_dims[1]
                                                  : x_dims[x_dims.size() - 1]);
  const int sample_size = x.numel() / N / C;

  // alloc memory
  dev_ctx.template Alloc<T>(y);

  using MPDType = typename MPTypeTrait<T>::Type;
  dev_ctx.template Alloc<MPDType>(mean_out);
  dev_ctx.template Alloc<MPDType>(variance_out);

  Tensor transformed_x;
  Tensor transformed_y;
  const int transformed_dim_size = 4;
  const int transformed_shape[transformed_dim_size] = {N, sample_size, 1, C};
  MLUCnnlTensorDesc transformed_desc(transformed_dim_size,
                                     transformed_shape,
                                     ToCnnlDataType<T>(),
                                     CNNL_LAYOUT_NHWC);
  MLUCnnlTensorDesc others_input_desc(scale);
  // input dimension is 2 and the format is NCHW. The input can be regarded as
  // NHWC format. Don't need to transpose.
  bool need_transpose =
      (data_layout == DataLayout::kNCHW && x_dims.size() != 2);
  if (need_transpose) {
    transformed_x.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_x);
    transformed_y.Resize(phi::DDim(transformed_shape, transformed_dim_size));
    dev_ctx.template Alloc<T>(&transformed_y);

    const std::vector<int> perm = {0, 2, 3, 1};
    const int x_reshaped[] = {N, C, sample_size, 1};
    MLUCnnlTensorDesc x_reshaped_desc(
        transformed_dim_size, x_reshaped, ToCnnlDataType<T>());
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_dim_size,
                       x_reshaped_desc.get(),
                       GetBasePtr(&x),
                       transformed_desc.get(),
                       GetBasePtr(&transformed_x));
  } else {
    transformed_x = x;
    transformed_y = *y;
  }

  MLUCnnl::FusedBatchNorm(dev_ctx,
                          false /* is_traing */,
                          transformed_desc.get(),
                          GetBasePtr(&transformed_x),
                          others_input_desc.get(),
                          GetBasePtr(&scale),
                          GetBasePtr(&bias),
                          GetBasePtr(&mean),
                          GetBasePtr(&variance),
                          epsilon,
                          momentum,
                          transformed_desc.get(),
                          GetBasePtr(&transformed_y),
                          GetBasePtr(mean_out),
                          GetBasePtr(variance_out),
                          nullptr,
                          nullptr);

  if (need_transpose) {
    const int y_reshaped[] = {N, C, sample_size, 1};
    MLUCnnlTensorDesc y_reshaped_desc(
        transformed_dim_size, y_reshaped, ToCnnlDataType<T>());
    const std::vector<int> perm = {0, 3, 1, 2};
    MLUCnnl::Transpose(dev_ctx,
                       perm,
                       transformed_y.dims().size(),
                       transformed_desc.get(),
                       GetBasePtr(&transformed_y),
                       y_reshaped_desc.get(),
                       GetBasePtr(y));
  }
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(batch_norm,
                          CustomMLU,
                          ALL_LAYOUT,
                          custom_kernel::BatchNormKernel,
                          phi::dtype::float16,
                          float) {}

PD_REGISTER_PLUGIN_KERNEL(batch_norm_grad,
                          CustomMLU,
                          ALL_LAYOUT,
                          custom_kernel::BatchNormGradKernel,
                          phi::dtype::float16,
                          float) {}

PD_REGISTER_PLUGIN_KERNEL(batch_norm_infer,
                          CustomMLU,
                          ALL_LAYOUT,
                          custom_kernel::BatchNormInferKernel,
                          phi::dtype::float16,
                          float) {}
