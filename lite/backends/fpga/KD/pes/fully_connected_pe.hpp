/* Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <vector>

#include "lite/backends/fpga/KD/pe.hpp"
#include "lite/backends/fpga/KD/pe_params.hpp"
#include "lite/backends/fpga/KD/pes/conv_pe.hpp"
#include "lite/backends/fpga/KD/pes/conv_process.hpp"
#include "lite/backends/fpga/KD/tensor_util.hpp"

namespace paddle {
namespace zynqmp {

class FullyConnectedPE : public PE {
 public:
  bool init() {
    Tensor* output = param_.output;
    output->setAligned(true);
    output->setDataLocation(Device);
    return true;
  }

  void apply() {
    ConvParam& convParam_ = convPE_.param();
    int num = param_.filter->shape().channel();
    int chw = param_.filter->shape().num();
    int in_channel = chw;

    Shape in_shape(NCHW, {1, in_channel, 1, 1});
    input_.mutableData<float16>(FP16, in_shape);

    convParam_.input = &input_;
    convParam_.output = param_.output;

    convParam_.activeParam.type = param_.activeParam.type;
    convParam_.groups = 1;
    convParam_.strides = {1, 1};
    convParam_.paddings = {0, 0};
    convParam_.kernelSize = {1, 1};
    convParam_.dilations = {1, 1};

    int height = input_.shape().height();
    int width = input_.shape().width();
    int filter_channel = chw / height / width;

    int channel = param_.output->shape().channel();
    Shape chw_shape(NCHW,
                    {num,
                     param_.input->shape().channel(),
                     param_.input->shape().height(),
                     param_.input->shape().width()});
    Shape shape(NCHW, {num, filter_channel, height, width});
    Tensor chw_filter;
    Tensor* conv_filter = &chw_filter;
    float* new_filter_data = conv_filter->mutableData<float>(FP32, chw_shape);
    float* filter_data = param_.filter->data<float>();

    for (int i = 0; i < num; i++) {
      for (int j = 0; j < chw; j++) {
        float scale = filter_data[j * num + i];
        new_filter_data[i * chw + j] = scale;
      }
    }

    chw_to_hwc(&chw_filter, &conv_filter_);
    conv_filter_.mutableData<float>(FP32, shape);
    convParam_.filter = &conv_filter_;
    convParam_.filter->flush();

    Shape sb_shape(N, {channel});
    float* scale_data = convParam_.scale()->mutableData<float>(FP32, sb_shape);
    float* bias_data = convParam_.bias()->mutableData<float>(FP32, sb_shape);

    for (int i = 0; i < channel; i++) {
      scale_data[i] = 1.0f;
      bias_data[i] = param_.bias->data<float>()[i];
    }
    convParam_.scale()->flush();
    convParam_.bias()->flush();

    convPE_.init();
    convPE_.apply();
  }

  void cpu_compute() {
    int num = param_.filter->shape().channel();
    int chw = param_.filter->shape().num();

    float* filter_data = param_.filter->data<float>();
    float max = 0.0f;
    Tensor* input = param_.input;
    Tensor* output = param_.output;
    float16* input_data = input->data<float16>();
    float16* output_data = output->data<float16>();

    for (int i = 0; i < num; i++) {
      float sum = 0;
      float bias = param_.bias->data<float>()[i];
      for (int j = 0; j < chw; j++) {
        float scale = filter_data[j * num + i];
        float data = half_to_float(input_data[j]);
        sum += scale * data;
      }
      output_data[i] = float_to_half(sum + bias);
      if (max < output_data[i]) {
        max = output_data[i];
      }
    }

    output->flush();
    output->scale()[0] = max / 127.0f;
    output->scale()[1] = 127.0f / max;
  }

  bool dispatch() {
    param_.input->invalidate();
    input_.copyFrom(param_.input);
    input_.flush();
    return convPE_.dispatch();
  }

  FullyConnectedParam& param() { return param_; }

 private:
  Tensor input_;
  Tensor conv_filter_;
  FullyConnectedParam param_;
  ConvPE convPE_;
};
}  // namespace zynqmp
}  // namespace paddle
