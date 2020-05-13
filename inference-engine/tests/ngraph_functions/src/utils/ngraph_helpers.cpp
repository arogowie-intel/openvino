// Copyright (C) 2019 Intel Corporationconvert2OutputVector
// SPDX-License-Identifier: Apache-2.0
//

#include <vector>
#include <memory>
#include <queue>

#include <ngraph/opsets/opset1.hpp>
#include <ngraph/opsets/opset3.hpp>
#include <ngraph/specialize_function.hpp>

#include <ngraph_functions/utils/ngraph_helpers.hpp>
#include <ngraph/opsets/opset.hpp>

namespace ngraph {
namespace helpers {

OutputVector convert2OutputVector(const std::vector<std::shared_ptr<Node>> &nodes) {
    OutputVector outs;
    std::for_each(nodes.begin(), nodes.end(), [&outs](const std::shared_ptr<Node> &n) {
        for (const auto &out_p : n->outputs()) {
            outs.push_back(out_p);
        }
    });
    return outs;
}

std::vector<std::vector<std::uint8_t>> interpreterFunction(const std::shared_ptr<Function> &function,
                                                           const std::vector<std::vector<std::uint8_t>> &inputs) {
    runtime::Backend::set_backend_shared_library_search_directory("");
    ngraph_register_interpreter_backend();
    auto backend = runtime::Backend::create("INTERPRETER");

    const auto &parameters = function->get_parameters();
    const auto &parametersNumber = parameters.size();
    const auto &inputsNumber = inputs.size();
    NGRAPH_CHECK(parametersNumber == inputsNumber,
                 "Got function (", function->get_friendly_name(), ") with ", parametersNumber, " parameters, but ",
                 inputsNumber, " input blobs");

    auto inputTensors = std::vector<std::shared_ptr<runtime::Tensor>>{};
    for (const auto &parameter : parameters) {
        const auto &parameterIndex = function->get_parameter_index(parameter);
        const auto &parameterShape = parameter->get_shape();
        const auto &parameterType = parameter->get_element_type();
        const auto &parameterSize = shape_size(parameterShape) * parameterType.size();

        const auto &input = inputs[parameterIndex];
        const auto &inputSize = input.size();
        NGRAPH_CHECK(parameterSize == inputSize,
                     "Got parameter (", parameter->get_friendly_name(), ") of size ", parameterSize,
                     " bytes, but corresponding input with index ", parameterIndex,
                     " has ", inputSize, " bytes");

        auto tensor = backend->create_tensor(parameterType, parameterShape);
        tensor->write(input.data(), parameterSize);
        inputTensors.push_back(tensor);
    }

    auto outputTensors = std::vector<std::shared_ptr<runtime::Tensor>>{};
    const auto &results = function->get_results();
    for (size_t i = 0; i <results.size(); ++i) {
        outputTensors.push_back(std::make_shared<HostTensor>());
    }

    auto handle = backend->compile(function);
    handle->call_with_validate(outputTensors, inputTensors);
    auto outputs = std::vector<std::vector<std::uint8_t>>(results.size());
    for (const auto &result : results) {
        const auto &resultIndex = function->get_result_index(result);
        auto &output = outputs[resultIndex];
        output.resize(shape_size(result->get_shape()) * result->get_element_type().size());
        outputTensors[resultIndex]->read(output.data(), output.size());
    }

    return outputs;
}

std::shared_ptr<Function> foldFunction(const std::shared_ptr<Function> &function,
                                       const std::vector<std::vector<std::uint8_t>> &inputs) {
    std::vector<element::Type> paramElementTypes;
    std::vector<PartialShape> paramShapes;
    for (const auto &param : function->get_parameters()) {
        paramElementTypes.emplace_back(param->get_element_type());
        paramShapes.emplace_back(param->get_shape());
    }

    auto inBuffers = std::vector<void *>(inputs.size());
    std::transform(inputs.cbegin(), inputs.cend(), inBuffers.begin(),
                   [](const std::vector<std::uint8_t> &input) {
                       // const_cast added to satisfy specialize_function interface
                       // which requires inputs as std::vector<void *>
                       return const_cast<std::uint8_t *>(input.data());
                   });

    const auto &foldedFunc = specialize_function(function, paramElementTypes, paramShapes, inBuffers, true, true);
    for (const auto &op : foldedFunc->get_ops()) {
        NGRAPH_CHECK(op->is_constant() || op->is_output() || op->is_parameter(),
                     "Function was not fully folded to constant state!\n",
                     "At least one non constant node with type ", op->get_type_name(),
                     " present in function.");
    }
    return foldedFunc;
}

std::vector<std::vector<std::uint8_t>> getConstData(const std::shared_ptr<Function> &function) {
    size_t numOutputs = function->get_output_size();
    auto outputs = std::vector<std::vector<std::uint8_t>>(numOutputs);
    for (size_t i = 0; i < numOutputs; i++) {
        const auto &output = function->output(i).get_node_shared_ptr();
        NGRAPH_CHECK(output->inputs().size() == 1);
        auto parrentNode = output->input_value(0).get_node_shared_ptr();
        NGRAPH_CHECK(parrentNode->is_constant(), "Function was not fully folded to constant state!\n",
                     "Parent node of one of results is not constant and has type ", parrentNode->get_type_name());
        const auto data = std::dynamic_pointer_cast<opset1::Constant>(parrentNode)->get_data_ptr<std::uint8_t>();
        const auto dataSize = shape_size(parrentNode->get_shape()) * parrentNode->get_element_type().size();
        outputs[i].resize(dataSize);
        std::copy(data, data + dataSize, outputs[i].data());
    }
    return outputs;
}

namespace {

using ComparingNodesPair = std::pair<std::shared_ptr<ngraph::Node>, std::shared_ptr<ngraph::Node>>;

std::string toString(const NodeTypeInfo& typeInfo) {
    return std::string(typeInfo.name) + " ver. " + std::to_string(typeInfo.version);
}

void CompareShapes(const PartialShape& actual, const PartialShape& expected) {
    NGRAPH_CHECK(actual.relaxes(expected) && actual.refines(expected), "Functions compare: Different shape detected ", actual, " and ", expected);
}

void CompareNodes(const Node& actual, const Node& expected) {
    const auto& actualType   = actual.get_type_info();
    const auto& expectedType = expected.get_type_info();
    NGRAPH_CHECK(actualType == expectedType, "Functions compare: data types must be equal ", toString(actualType), " != ", toString(expectedType));

    const auto& numActualInputs = actual.inputs().size();
    const auto& numExpectedInputs = expected.inputs().size();
    NGRAPH_CHECK(numActualInputs == numExpectedInputs, "Functions compare: numbers of inputs are different: ", numActualInputs, " and ", numExpectedInputs);
}

}  // namespace

void CompareFunctions(const Function& actual, const Function& expected) {
    const auto& actualResults = actual.get_results();
    NGRAPH_CHECK(actualResults.size() == 1, "Got ", actualResults.size(), " outputs for function, but only single output functions are supported");
    const auto& actualResult = actualResults.front();

    const auto& expectedResults = expected.get_results();
    NGRAPH_CHECK(expectedResults.size() == 1, "Got ", expectedResults.size(), " outputs for function, but only single output functions are supported");
    const auto& expectedResult = expectedResults.front();

    std::queue<ComparingNodesPair> nodes;
    nodes.emplace(actualResult, expectedResult);
    while (!nodes.empty()) {
        const auto& checkingNodes = nodes.front();
        const auto& actualNode    = checkingNodes.first;
        const auto& expectedNode  = checkingNodes.second;
        nodes.pop();

        CompareNodes(*actualNode, *expectedNode);

        for (std::size_t i = 0; i < actualNode->inputs().size(); ++i) {
            const auto& actualShape = actualNode->input(i).get_partial_shape();
            const auto& expectedShape = expectedNode->input(i).get_partial_shape();
            CompareShapes(actualShape, expectedShape);

            nodes.emplace(actualNode->input_value(i).get_node_shared_ptr(), expectedNode->input_value(i).get_node_shared_ptr());
        }
    }
}

std::shared_ptr<ngraph::Node> getNodeSharedPtr(const ngraph::NodeTypeInfo &type_info, const ngraph::OutputVector &outputVector) {
    for (const auto& opset : {ngraph::get_opset3(), ngraph::get_opset2(), ngraph::get_opset1()})
        if (opset.contains_type(type_info)) {
            const auto ngraphNode = std::shared_ptr<ngraph::Node>(opset.create(type_info.name));
            ngraphNode->set_arguments(outputVector);
            ngraphNode->validate_and_infer_types();
            return ngraphNode;
        }
    NGRAPH_UNREACHABLE("supported opsets does not contain op with name: ", type_info.name, " version: ", type_info.version);
}

}  // namespace helpers
}  // namespace ngraph