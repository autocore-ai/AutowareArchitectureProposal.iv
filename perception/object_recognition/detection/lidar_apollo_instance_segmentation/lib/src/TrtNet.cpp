/*
 * MIT License
 *
 * Copyright (c) 2018 lewes6369
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/
#include "TrtNet.hpp"
#include "cublas_v2.h"
#include "cudnn.h"
#include <string.h>
#include <time.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <sstream>
#include <unordered_map>

using namespace nvinfer1;
using namespace nvcaffeparser1;
using namespace plugin;

static Tn::Logger gLogger;

#define RETURN_AND_LOG(ret, severity, message)                            \
  do {                                                                    \
    std::string error_message = "ssd_error_log: " + std::string(message); \
    gLogger.log(ILogger::Severity::k##severity, error_message.c_str());   \
    return (ret);                                                         \
  } while (0)


inline void * safeCudaMalloc(size_t memSize)
{
  void * deviceMem;
  CUDA_CHECK(cudaMalloc(&deviceMem, memSize));
  if (deviceMem == nullptr) {
    std::cerr << "Out of memory" << std::endl;
    exit(1);
  }
  return deviceMem;
}

inline int64_t volume(const nvinfer1::Dims & d)
{
  return std::accumulate(d.d, d.d + d.nbDims, 1, std::multiplies<int64_t>());
}

inline unsigned int getElementSize(nvinfer1::DataType t)
{
  switch (t) {
    case nvinfer1::DataType::kINT32:
      return 4;
    case nvinfer1::DataType::kFLOAT:
      return 4;
    case nvinfer1::DataType::kHALF:
      return 2;
    case nvinfer1::DataType::kINT8:
      return 1;
    default:
      throw std::runtime_error("Invalid DataType.");
      return 0;
  }
}

namespace Tn
{
trtNet::trtNet(
  const std::string & prototxt, const std::string & caffemodel,
  const std::vector<std::string> & outputNodesName,
  const std::vector<std::vector<float>> & calibratorData, RUN_MODE mode /*= RUN_MODE::FLOAT32*/,bool enable_dla)
: mTrtContext(nullptr),
  mTrtEngine(nullptr),
  mTrtRunTime(nullptr),
  mTrtRunMode(mode),
  mTrtInputCount(0),
  enable_dla_(enable_dla),
  mTrtIterationTime(0)
{
  std::cout << "init plugin proto: " << prototxt << " caffemodel: " << caffemodel << std::endl;
  auto parser = createCaffeParser();

  const int maxBatchSize = 1;
  IHostMemory * trtModelStream{nullptr};

  ICudaEngine * tmpEngine = loadModelAndCreateEngine(
    prototxt.c_str(), caffemodel.c_str(), maxBatchSize, parser, trtModelStream, outputNodesName);
  assert(tmpEngine != nullptr);
  assert(trtModelStream != nullptr);
  tmpEngine->destroy();
  
  mTrtRunTime = createInferRuntime(gLogger);
  assert(mTrtRunTime != nullptr);
  mTrtEngine = mTrtRunTime->deserializeCudaEngine(
    trtModelStream->data(), trtModelStream->size(), nullptr);
  assert(mTrtEngine != nullptr);
  // Deserialize the engine.
  trtModelStream->destroy();

  InitEngine();
}

trtNet::trtNet(const std::string & engineFile)
: mTrtContext(nullptr),
  mTrtEngine(nullptr),
  mTrtRunTime(nullptr),
  mTrtRunMode(RUN_MODE::FLOAT32),
  mTrtInputCount(0)
{
  using namespace std;
  fstream file;

  file.open(engineFile, ios::binary | ios::in);
  if (!file.is_open()) {
    cout << "read engine file" << engineFile << " failed" << endl;
    return;
  }
  file.seekg(0, ios::end);
  int length = file.tellg();
  file.seekg(0, ios::beg);
  std::unique_ptr<char[]> data(new char[length]);
  file.read(data.get(), length);
  file.close();

  mTrtRunTime = createInferRuntime(gLogger);
  assert(mTrtRunTime != nullptr);
  mTrtEngine = mTrtRunTime->deserializeCudaEngine(data.get(), length, nullptr);
  assert(mTrtEngine != nullptr);

  InitEngine();
}

void trtNet::InitEngine()
{
  const int maxBatchSize = 1;
  mTrtContext = mTrtEngine->createExecutionContext();
  assert(mTrtContext != nullptr);
  mTrtContext->setProfiler(&mTrtProfiler);

  int nbBindings = mTrtEngine->getNbBindings();

  mTrtCudaBuffer.resize(nbBindings);
  mTrtBindBufferSize.resize(nbBindings);
  for (int i = 0; i < nbBindings; ++i) {
    Dims dims = mTrtEngine->getBindingDimensions(i);
    DataType dtype = mTrtEngine->getBindingDataType(i);
    int64_t totalSize = volume(dims) * maxBatchSize * getElementSize(dtype);
    mTrtBindBufferSize[i] = totalSize;
    mTrtCudaBuffer[i] = safeCudaMalloc(totalSize);
    if (mTrtEngine->bindingIsInput(i)) {mTrtInputCount++;}
  }

  CUDA_CHECK(cudaStreamCreate(&mTrtCudaStream));
}

void trtNet::doInference(const void * inputData, void * outputData)
{
  static const int batchSize = 1;
  assert(mTrtInputCount == 1);

  int inputIndex = 0;
  CUDA_CHECK(
    cudaMemcpyAsync(
      mTrtCudaBuffer[inputIndex], inputData, mTrtBindBufferSize[inputIndex], cudaMemcpyHostToDevice,
      mTrtCudaStream));

  auto t_start = std::chrono::high_resolution_clock::now();  
  mTrtContext->execute(batchSize, &mTrtCudaBuffer[inputIndex]);
  auto t_end = std::chrono::high_resolution_clock::now();
  float total = std::chrono::duration<float, std::milli>(t_end - t_start).count();
  if(0) //default does not print
  {
    std::cout << "Time taken for inference is " << total << " ms." << std::endl; 
  }
  for (size_t bindingIdx = mTrtInputCount; bindingIdx < mTrtBindBufferSize.size(); ++bindingIdx) {
    auto size = mTrtBindBufferSize[bindingIdx];
    CUDA_CHECK(
      cudaMemcpyAsync(
        outputData, mTrtCudaBuffer[bindingIdx], size, cudaMemcpyDeviceToHost, mTrtCudaStream));
  }
}


nvinfer1::ICudaEngine * trtNet::loadModelAndCreateEngine(
  const char * deployFile, const char * modelFile, int maxBatchSize, ICaffeParser * parser,
  IHostMemory *& trtModelStream, const std::vector<std::string> & outputNodesName)
{
  // Create the builder
  IBuilder * builder = createInferBuilder(gLogger);
  IBuilderConfig * config = builder->createBuilderConfig();

  // Parse the model to populate the network, then set the outputs.
  INetworkDefinition * network = builder->createNetworkV2(0U);

  std::cout << "Begin parsing model..." << std::endl;
  const IBlobNameToTensor * blobNameToTensor =
    parser->parse(deployFile, modelFile, *network, nvinfer1::DataType::kFLOAT);
  if (!blobNameToTensor) 
  {
    RETURN_AND_LOG(nullptr, ERROR, "Fail to parse");
    // cout<<"Fail to parse"<<endl;
    return nullptr;
  }
  std::cout << "End parsing model..." << std::endl;

  // specify which tensors are outputs
  for (auto & name : outputNodesName) {
    auto output = blobNameToTensor->find(name.c_str());
    assert(output != nullptr);
    if (output == nullptr) std::cout << "can not find output named " << name << std::endl;

    network->markOutput(*output);
  }

  // Build the engine.
  builder->setMaxBatchSize(maxBatchSize);
  config->setMaxWorkspaceSize(1 << 30);  // 1G
  if (mTrtRunMode == RUN_MODE::INT8) {
    std::cout << "setInt8Mode" << std::endl;
    if (!builder->platformHasFastInt8())
      std::cout << "Notice: the platform do not has fast for int8" << std::endl;
    config->setFlag(BuilderFlag::kINT8);
    // config->setInt8Calibrator(calibrator);
  } else if (mTrtRunMode == RUN_MODE::FLOAT16) {
    std::cout << "setFp16Mode" << std::endl;
    if (!builder->platformHasFastFp16())
      std::cout << "Notice: the platform do not has fast for fp16" << std::endl;
    config->setFlag(BuilderFlag::kFP16);
  }else
  {
      std::cout << "setFp32Mode" << std::endl;
  }
  
  if(enable_dla_)
  {
    std::cout<<"enable DLA"<<std::endl;
    config->setFlag(BuilderFlag::kGPU_FALLBACK); //无法跑在DLA上则退回到GPU上
    config->setDefaultDeviceType(DeviceType::kDLA);
    int dlaCore = 0;//跑在哪个DLA核上. xavier一共有两个DLA
    config->setDLACore(dlaCore);

    int nbLayers = network->getNbLayers();
    int layersOnDLA = 0;
    std::cout << "Total number of layers: " << nbLayers << std::endl;
    for (int i = 0; i < nbLayers; i++)
    {
        nvinfer1::ILayer* curLayer = network->getLayer(i);
        if(config->canRunOnDLA(curLayer))
        {
            config->setFlag(BuilderFlag::kFP16);
            config->setDeviceType(curLayer, nvinfer1::DeviceType::kDLA);
            layersOnDLA++;
            std::cout << "Set layer " << curLayer->getName() << " to run on DLA" << std::endl;
        }
    }
    std::cout << "Total number of layers on DLA: " << layersOnDLA << std::endl;
  }
   
  std::cout << "Begin building engine..." << std::endl;
  ICudaEngine * engine = builder->buildEngineWithConfig(*network, *config);
  if (!engine) RETURN_AND_LOG(nullptr, ERROR, "Unable to create engine");
  std::cout << "End building engine..." << std::endl;

  // We don't need the network any more, and we can destroy the parser.
  network->destroy();
  parser->destroy();

  // Serialize the engine, then close everything down.
  trtModelStream = engine->serialize();

  builder->destroy();
  config->destroy();
  shutdownProtobufLibrary();
  return engine;
}

}  // namespace Tn
