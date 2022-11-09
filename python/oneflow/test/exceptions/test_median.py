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

import unittest

import oneflow as flow
import oneflow.unittest


@flow.unittest.skip_unless_1n1d()
class TestMedian(flow.unittest.TestCase):
    def test_median_exception_dim_out_of_range(test_case):
        x = flow.tensor((2, 2))
        with test_case.assertRaises(IndexError) as ctx:
            y = flow.median(x, 1)
        test_case.assertTrue(
            "Dimension out of range (expected to be in range of [-1, 0], but got 1)"
            in str(ctx.exception)
        )

    def test_median_exception_reduce_0dim(test_case):
        x = flow.randn(2, 0, 2)
        with test_case.assertRaises(IndexError) as ctx:
            y = flow.median(x, 1)
        test_case.assertTrue(
            "IndexError: Expected reduction dim 1 to have non-zero size."
            in str(ctx.exception)
        )


if __name__ == "__main__":
    unittest.main()
