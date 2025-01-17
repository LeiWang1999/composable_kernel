// SPDX-License-Identifier: MIT
// Copyright (c) 2024, Advanced Micro Devices, Inc. All rights reserved.

#include <vector>
#include <iostream>
#include <numeric>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <time.h>
#include <unordered_set>

#include "ck_tile/core.hpp"
#include "ck_tile/ops/reduce.hpp"
#include "topk_softmax_api.hpp"

#if 0
template <typename T>
void dump_host_tensor_2d(const ck_tile::HostTensor<T>& x)
{
    auto len = x.get_lengths();
    assert(len.size() == 2);
    std::cout << "[";
    for(size_t i = 0; i < len[0]; i++)
    {
        std::cout << i << ": [";
        for(size_t j = 0; j < len[1]; j++)
        {
            if constexpr(std::is_same_v<T, ck_tile::fp16_t>)
            {
                auto v = ck_tile::type_convert<float>(x(i, j));

                std::cout << v;
                if(j != len[1] - 1)
                    std::cout << ",";
            }
            else
            {
                std::cout << x(i, j) << " ";
            }
        }
        std::cout << "]";
        if(i != len[0] - 1)
            std::cout << ",";
        else
            std::cout << "]";
        std::cout << std::endl;
    }
    std::cout << "--------------------" << std::endl;
}
#endif

// CPU reference
template <typename InputType, typename WeightType, typename IndexType = ck_tile::index_t>
auto reference_topk_softmax(const ck_tile::HostTensor<InputType>& x,
                            ck_tile::index_t k,
                            ck_tile::index_t dim = -1,
                            bool largest         = true,
                            bool sorted          = true)
{
    using namespace ck_tile;

    auto y = reference_softmax<InputType, WeightType, WeightType>(x, dim);

    auto [y_values, y_indices] = reference_topk(y, k, dim, largest, sorted);

    return ck_tile::make_tuple(y_values, y_indices);
}

template <typename InputType, typename WeightType, typename IndexType = ck_tile::index_t>
auto reference_topk_softmax(const ck_tile::HostTensor<InputType>& x,
                            ck_tile::HostTensor<WeightType>& y_values,
                            ck_tile::HostTensor<IndexType>& y_indices,
                            ck_tile::index_t k,
                            ck_tile::index_t dim = -1,
                            bool largest         = true,
                            bool sorted          = true)
{
    using namespace ck_tile;

    auto y = reference_softmax<InputType, WeightType, WeightType>(x, dim);
    reference_topk(y, y_values, y_indices, k, dim, largest, sorted);
}

// different threshold for different dtype
template <typename DataType>
auto get_elimit(std::string /*init_method*/)
{
    double rtol = 1e-3;
    double atol = 1e-3;
    return ck_tile::make_tuple(rtol, atol);
}

template <>
auto get_elimit<ck_tile::bf16_t>(std::string /*init_method*/)
{
    double rtol = 1e-2;
    double atol = 1e-2;
    return ck_tile::make_tuple(rtol, atol);
}

template <>
auto get_elimit<ck_tile::fp8_t>(std::string init_method)
{
    if(init_method == "ui" || init_method == "ni")
    {
        unsigned max_rounding_point_distance = 0;
        double atol                          = 2e-3;
        return ck_tile::make_tuple(max_rounding_point_distance, atol);
    }
    else
    {
        unsigned max_rounding_point_distance = 1;
        double atol                          = 0.0625;
        return ck_tile::make_tuple(max_rounding_point_distance, atol);
    }
}

auto create_args(int argc, char* argv[])
{
    ck_tile::ArgParser arg_parser;
    arg_parser.insert("v", "1", "weather do CPU validation or not")
        .insert("pr_i", "fp16", "input data type. fp16/fp32 (representing 8/16/32 bit data)")
        .insert("pr_w", "fp32", "output weight data type(currently only fp32 supported now)")
        .insert("t", "32", "number of input tokens")
        .insert("e", "8", "number of experts")
        .insert("k", "2", "topk")
        .insert("st_i", "-1", "row stride of input, -1 means same as experts")
        .insert("st_o", "-1", "row stride of output/indices, -1 means same as topk")
        .insert("seed", "-1", "seed to be used, -1 means random every time")
        .insert("kname", "0", "when set to 1 it will print kernel name")
        .insert("warmup", "5", "number of iterations before benchmark the kernel")
        .insert("repeat", "20", "number of iterations to benchmark the kernel");

    bool result = arg_parser.parse(argc, argv);
    return std::make_tuple(result, arg_parser);
}

template <typename InputType, typename WeightType, typename IndexType = ck_tile::index_t>
bool test_topk_softmax(ck_tile::ArgParser args)
{
    int validate            = args.get_int("v");
    std::string input_prec  = args.get_str("pr_i");
    std::string weight_prec = args.get_str("pr_w");
    int tokens              = args.get_int("t");
    int experts             = args.get_int("e");
    int topk                = args.get_int("k");
    int seed                = args.get_int("seed");
    int stride_input        = args.get_int("st_i");
    int stride_output       = args.get_int("st_o");
    int kname               = args.get_int("kname");
    int warmup              = args.get_int("warmup");
    int repeat              = args.get_int("repeat");

    if(stride_input < 0)
    {
        stride_input = experts;
    }
    if(stride_output < 0)
    {
        stride_output = topk;
    }
    assert(stride_input >= experts);
    assert(stride_output >= topk);

    if(seed < 0)
    {
        seed = std::time(nullptr);
    }

    if(topk > experts)
    {
        printf("topk:%d value should be smaller than, or equal to number of experts:%d\n",
               topk,
               experts);
        return false;
    }

    // tokens already considered batch size
    ck_tile::HostTensor<InputType> x_host({tokens, experts}, {stride_input, 1});
    ck_tile::HostTensor<WeightType> value_host({tokens, topk}, {stride_output, 1});
    ck_tile::HostTensor<IndexType> index_host({tokens, topk}, {stride_output, 1});

    {
        // random require per-row unique
        auto rand_gen = ck_tile::FillUniformDistribution_Unique<InputType>{
            -5.f, 5.f, static_cast<uint32_t>(seed)};

        for(int i_t = 0; i_t < tokens; i_t++)
        {
            ck_tile::HostTensor<InputType> x_row({experts});
            rand_gen(x_row);
            std::copy(x_row.begin(), x_row.end(), x_host.begin() + i_t * stride_input);
            rand_gen.clear();
        }
    }

    ck_tile::DeviceMem x_dev(x_host.get_element_space_size_in_bytes());
    ck_tile::DeviceMem value_dev(value_host.get_element_space_size_in_bytes());
    ck_tile::DeviceMem index_dev(index_host.get_element_space_size_in_bytes());

    x_dev.ToDevice(x_host.data());

    topk_softmax_trait trait{input_prec, weight_prec, experts};

    topk_softmax_kargs karg{x_dev.GetDeviceBuffer(),
                            value_dev.GetDeviceBuffer(),
                            index_dev.GetDeviceBuffer(),
                            tokens,
                            experts,
                            topk,
                            stride_input,
                            stride_output};

    ck_tile::stream_config sc{nullptr,
                              true,
                              /* log_level = */ (kname ? 1 : 0),
                              warmup,
                              repeat};
    auto ms = topk_softmax(trait, karg, sc);
    printf("[%s|%s]tokens:%d, experts:%d, topk:%d, st_i:%d, st_o:%d, ms:%f, ",
           input_prec.c_str(),
           weight_prec.c_str(),
           tokens,
           experts,
           topk,
           stride_input,
           stride_output,
           ms);
    if(ms < 0)
        printf("not supported\n");
    fflush(stdout);
    if(ms < 0)
    {
        return false;
    }

    value_dev.FromDevice(value_host.data());
    index_dev.FromDevice(index_host.data());

    bool rtn = true;
    if(validate)
    {
        ck_tile::HostTensor<WeightType> value_ref({tokens, topk}, {stride_output, 1});
        ck_tile::HostTensor<IndexType> index_ref({tokens, topk}, {stride_output, 1});

        reference_topk_softmax<InputType, WeightType, IndexType>(
            x_host, value_ref, index_ref, topk);

        auto [rtol, atol] = get_elimit<InputType>("");
        for(int i_t = 0; i_t < tokens; i_t++)
        {
            auto s_begin = std::vector<size_t>{static_cast<size_t>(i_t), static_cast<size_t>(0)};
            auto s_end =
                std::vector<size_t>{static_cast<size_t>(i_t + 1), static_cast<size_t>(topk)};
            auto s_value_host = value_host.slice(s_begin, s_end);
            auto s_value_ref  = value_ref.slice(s_begin, s_end);
            rtn &= ck_tile::check_err(s_value_host,
                                      s_value_ref,
                                      std::string("[") + std::to_string(i_t) +
                                          std::string("] Value Error:"),
                                      rtol,
                                      atol);
            auto s_index_host = index_host.slice(s_begin, s_end);
            auto s_index_ref  = index_ref.slice(s_begin, s_end);
            rtn &= ck_tile::check_err(s_index_host,
                                      s_index_ref,
                                      std::string("[") + std::to_string(i_t) +
                                          std::string("] Index Error:"),
                                      rtol,
                                      atol);
        }
    }

    printf("valid:%s\n", rtn ? "y" : "n");
    fflush(stdout);
    return rtn;
}

int main(int argc, char** argv)
{
    auto [result, args] = create_args(argc, argv);
    if(!result)
        return -1;
    std::string input_prec  = args.get_str("pr_i");
    std::string weight_prec = args.get_str("pr_w");

    bool r = true;
    if(input_prec.compare("fp16") == 0 && weight_prec.compare("fp32") == 0)
    {
        r &= test_topk_softmax<ck_tile::fp16_t, float, ck_tile::index_t>(args);
    }
    else if(input_prec.compare("bf16") == 0 && weight_prec.compare("fp32") == 0)
    {
        r &= test_topk_softmax<ck_tile::bf16_t, float, ck_tile::index_t>(args);
    }

    return r ? 0 : -1;
}
