"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
import os
from collections import OrderedDict

import numpy as np
import oneflow as flow
import test_global_storage
from test_util import GenArgList


def TestMultiInput(x1, x2):
    return (
        flow.user_op_builder("my_test_multi_input")
        .Op("TestMultiInput")
        .Input("x1", [x1])
        .Input("x2", [x2])
        .Output("y")
        .Build()
        .InferAndTryRun()
        .RemoteBlobList()[0]
    )


def test_TestMultiInput_grad_mirrored_inplace(test_case):
    func_config = flow.FunctionConfig()
    func_config.default_data_type(flow.float)
    func_config.default_distribute_strategy(flow.scope.mirrored_view())
    func_config.train.primary_lr(1e-4)
    func_config.train.model_update_conf(dict(naive_conf={}))

    shape = (
        3,
        3,
    )

    @flow.global_function(func_config)
    def TestMultiInputJob():
        with flow.scope.placement("gpu", "0:0"):
            x1 = flow.get_variable(
                "x1",
                shape=shape,
                dtype=flow.float,
                initializer=flow.random_uniform_initializer(minval=-10, maxval=10),
                trainable=True,
            )
            x2 = flow.get_variable(
                "x2",
                shape=shape,
                dtype=flow.float,
                initializer=flow.random_uniform_initializer(minval=-10, maxval=10),
                trainable=True,
            )
            loss = TestMultiInput(x1, x2)
            flow.losses.add_loss(loss)

            flow.watch(x1, test_global_storage.Setter("x1"))
            flow.watch_diff(x1, test_global_storage.Setter("x1_diff"))
            flow.watch(x2, test_global_storage.Setter("x2"))
            flow.watch_diff(x2, test_global_storage.Setter("x2_diff"))
            return loss

    check_point = flow.train.CheckPoint()
    check_point.init()
    out = TestMultiInputJob().get()
    x1_diff = test_global_storage.Get("x1_diff")
    x2_diff = test_global_storage.Get("x2_diff")

    expect_out = test_global_storage.Get("x1")
    expect_x1_diff = np.ones(shape, dtype=np.float32)
    expect_x2_diff = np.ones(shape, dtype=np.float32) * 2.0
    # print(x1_diff, x2_diff)
    # print(expect_x1_diff, expect_x2_diff)
    assert np.allclose(out.numpy(), expect_out)
    assert np.allclose(x1_diff, expect_x1_diff)
    assert np.allclose(x2_diff, expect_x2_diff)
