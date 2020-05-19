// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

/**
* \brief The entry point for the Inference Engine object_detection demo application
* \file object_detection_demo_ssd_async/main.cpp
* \example object_detection_demo_ssd_async/main.cpp
*/

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <algorithm>

#include <ngraph/ngraph.hpp>

#include <monitors/presenter.h>
#include <samples/ocv_common.hpp>
#include <samples/args_helper.hpp>
#include <cldnn/cldnn_config.hpp>

#include "object_detection_demo_ssd_async.hpp"

using namespace InferenceEngine;

bool ParseAndCheckCommandLine(int argc, char *argv[]) {
    // ---------------------------Parsing and validation of input args--------------------------------------
    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_h) {
       showUsage();
       showAvailableDevices();
       return false;
    }
    slog::info << "Parsing input parameters" << slog::endl;

    if (FLAGS_i.empty()) {
        throw std::logic_error("Parameter -i is not set");
    }

    if (FLAGS_m.empty()) {
        throw std::logic_error("Parameter -m is not set");
    }

    return true;
}

void frameToBlob(const cv::Mat& frame,
                 InferRequest::Ptr& inferRequest,
                 const std::string& inputName) {
    if (FLAGS_auto_resize) {
        /* Just set input blob containing read image. Resize and layout conversion will be done automatically */
        inferRequest->SetBlob(inputName, wrapMat2Blob(frame));
    } else {
        /* Resize and copy data from the image to the input blob */
        Blob::Ptr frameBlob = inferRequest->GetBlob(inputName);
        matU8ToBlob<uint8_t>(frame, frameBlob);
    }
}

int main(int argc, char *argv[]) {
    try {
        /** This demo covers certain topology and cannot be generalized for any object detection **/
        std::cout << "InferenceEngine: " << GetInferenceEngineVersion() << std::endl;

        // ------------------------------ Parsing and validation of input args ---------------------------------
        if (!ParseAndCheckCommandLine(argc, argv)) {
            return 0;
        }

        slog::info << "Reading input" << slog::endl;
        cv::VideoCapture cap;
        if (!((FLAGS_i == "cam") ? cap.open(0) : cap.open(FLAGS_i.c_str()))) {
            throw std::logic_error("Cannot open input file or camera: " + FLAGS_i);
        }
        const size_t width  = (size_t)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        const size_t height = (size_t)cap.get(cv::CAP_PROP_FRAME_HEIGHT);

        // read input (video) frame
        cv::Mat curr_frame;  cap >> curr_frame;
        cv::Mat next_frame;

        if (!cap.grab()) {
            throw std::logic_error("This demo supports only video (or camera) inputs !!! "
                                   "Failed getting next frame from the " + FLAGS_i);
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 1. Load inference engine ------------------------------------------------
        slog::info << "Loading Inference Engine" << slog::endl;
        Core ie;

        slog::info << "Device info: " << slog::endl;
        std::cout << ie.GetVersions(FLAGS_d);

        /** Load extensions for the plugin **/
        if (!FLAGS_l.empty()) {
            // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
            IExtensionPtr extension_ptr = make_so_pointer<IExtension>(FLAGS_l.c_str());
            ie.AddExtension(extension_ptr, "CPU");
        }
        if (!FLAGS_c.empty()) {
            // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
            ie.SetConfig({{PluginConfigParams::KEY_CONFIG_FILE, FLAGS_c}}, "GPU");
        }

        /** Per layer metrics **/
        if (FLAGS_pc) {
            ie.SetConfig({ { PluginConfigParams::KEY_PERF_COUNT, PluginConfigParams::YES } });
        }

        std::map<std::string, std::string> userSpecifiedConfig;
        std::map<std::string, std::string> minLatencyConfig;

        std::set<std::string> devices;
        for (const std::string& device : parseDevices(FLAGS_d)) {
            devices.insert(device);
        }
        std::map<std::string, unsigned> deviceNstreams = parseValuePerDevice(devices, FLAGS_nstreams);
        for (auto & device : devices) {
            if (device == "CPU") {  // CPU supports a few special performance-oriented keys
                // limit threading for CPU portion of inference
                if (FLAGS_nthreads != 0)
                    userSpecifiedConfig.insert({ CONFIG_KEY(CPU_THREADS_NUM), std::to_string(FLAGS_nthreads) });

                if (FLAGS_d.find("MULTI") != std::string::npos
                    && devices.find("GPU") != devices.end()) {
                    userSpecifiedConfig.insert({ CONFIG_KEY(CPU_BIND_THREAD), CONFIG_VALUE(NO) });
                } else {
                    // pin threads for CPU portion of inference
                    userSpecifiedConfig.insert({ CONFIG_KEY(CPU_BIND_THREAD), CONFIG_VALUE(YES) });
                }

                // for CPU execution, more throughput-oriented execution via streams
                userSpecifiedConfig.insert({ CONFIG_KEY(CPU_THROUGHPUT_STREAMS),
                                (deviceNstreams.count(device) > 0 ? std::to_string(deviceNstreams.at(device))
                                                                  : CONFIG_VALUE(CPU_THROUGHPUT_AUTO)) });

                minLatencyConfig.insert({ CONFIG_KEY(CPU_THROUGHPUT_STREAMS), "1" });

                deviceNstreams[device] = std::stoi(
                    ie.GetConfig(device, CONFIG_KEY(CPU_THROUGHPUT_STREAMS)).as<std::string>());
            } else if (device == "GPU") {
                userSpecifiedConfig.insert({ CONFIG_KEY(GPU_THROUGHPUT_STREAMS),
                                (deviceNstreams.count(device) > 0 ? std::to_string(deviceNstreams.at(device))
                                                                  : CONFIG_VALUE(GPU_THROUGHPUT_AUTO)) });

                minLatencyConfig.insert({ CONFIG_KEY(GPU_THROUGHPUT_STREAMS), "1" });

                deviceNstreams[device] = std::stoi(
                    ie.GetConfig(device, CONFIG_KEY(GPU_THROUGHPUT_STREAMS)).as<std::string>());

                if (FLAGS_d.find("MULTI") != std::string::npos
                    && devices.find("CPU") != devices.end()) {
                    // multi-device execution with the CPU + GPU performs best with GPU throttling hint,
                    // which releases another CPU thread (that is otherwise used by the GPU driver for active polling)
                    userSpecifiedConfig.insert({ CLDNN_CONFIG_KEY(PLUGIN_THROTTLE), "1" });
                }
            }
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
        slog::info << "Loading network files" << slog::endl;
        /** Read network model **/
        auto cnnNetwork = ie.ReadNetwork(FLAGS_m);
        /** Set batch size to 1 **/
        slog::info << "Batch size is forced to 1." << slog::endl;
        cnnNetwork.setBatchSize(1);
        /** Read labels (if any)**/
        std::string labelFileName = fileNameNoExt(FLAGS_m) + ".labels";
        std::vector<std::string> labels;
        std::ifstream inputFile(labelFileName);
        std::copy(std::istream_iterator<std::string>(inputFile),
                  std::istream_iterator<std::string>(),
                  std::back_inserter(labels));
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 3. Configure input & output ---------------------------------------------
        // --------------------------- Prepare input blobs -----------------------------------------------------
        slog::info << "Checking that the inputs are as the demo expects" << slog::endl;
        InputsDataMap inputInfo(cnnNetwork.getInputsInfo());

        std::string imageInputName, imageInfoInputName;
        size_t netInputHeight, netInputWidth;

        for (const auto & inputInfoItem : inputInfo) {
            if (inputInfoItem.second->getTensorDesc().getDims().size() == 4) {  // 1st input contains images
                imageInputName = inputInfoItem.first;
                inputInfoItem.second->setPrecision(Precision::U8);
                if (FLAGS_auto_resize) {
                    inputInfoItem.second->getPreProcess().setResizeAlgorithm(ResizeAlgorithm::RESIZE_BILINEAR);
                    inputInfoItem.second->getInputData()->setLayout(Layout::NHWC);
                } else {
                    inputInfoItem.second->getInputData()->setLayout(Layout::NCHW);
                }
                const TensorDesc& inputDesc = inputInfoItem.second->getTensorDesc();
                netInputHeight = getTensorHeight(inputDesc);
                netInputWidth = getTensorWidth(inputDesc);
            } else if (inputInfoItem.second->getTensorDesc().getDims().size() == 2) {   // 2nd input contains image info
                imageInfoInputName = inputInfoItem.first;
                inputInfoItem.second->setPrecision(Precision::FP32);
            } else {
                throw std::logic_error("Unsupported " +
                                       std::to_string(inputInfoItem.second->getTensorDesc().getDims().size()) + "D "
                                       "input layer '" + inputInfoItem.first + "'. "
                                       "Only 2D and 4D input layers are supported");
            }
        }

        // --------------------------- Prepare output blobs -----------------------------------------------------
        slog::info << "Checking that the outputs are as the demo expects" << slog::endl;
        OutputsDataMap outputInfo(cnnNetwork.getOutputsInfo());
        if (outputInfo.size() != 1) {
            throw std::logic_error("This demo accepts networks having only one output");
        }
        DataPtr& output = outputInfo.begin()->second;
        auto outputName = outputInfo.begin()->first;

        int num_classes = 0;

        if (auto ngraphFunction = cnnNetwork.getFunction()) {
            for (const auto op : ngraphFunction->get_ops()) {
                if (op->get_friendly_name() == outputName) {
                    auto detOutput = std::dynamic_pointer_cast<ngraph::op::DetectionOutput>(op);
                    if (!detOutput) {
                        THROW_IE_EXCEPTION << "Object Detection network output layer(" + op->get_friendly_name() +
                            ") should be DetectionOutput, but was " +  op->get_type_info().name;
                    }

                    num_classes = detOutput->get_attrs().num_classes;
                    break;
                }
            }
        } else if (!labels.empty()) {
            throw std::logic_error("Class labels are not supported with IR version older than 10");
        }

        if (static_cast<int>(labels.size()) != num_classes) {
            if (static_cast<int>(labels.size()) == (num_classes - 1))  // if network assumes default "background" class,
                labels.insert(labels.begin(), "fake");                 // having no label
            else
                labels.clear();
        }
        const SizeVector outputDims = output->getTensorDesc().getDims();
        const int maxProposalCount = outputDims[2];
        const int objectSize = outputDims[3];
        if (objectSize != 7) {
            throw std::logic_error("Output should have 7 as a last dimension");
        }
        if (outputDims.size() != 4) {
            throw std::logic_error("Incorrect output dimensions for SSD");
        }
        output->setPrecision(Precision::FP32);
        output->setLayout(Layout::NCHW);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 4. Loading model to the device ------------------------------------------
        slog::info << "Loading model to the device" << slog::endl;
        ExecutableNetwork userSpecifiedExecNetwork = ie.LoadNetwork(cnnNetwork, FLAGS_d, userSpecifiedConfig);
        ExecutableNetwork minLatencyExecNetwork = ie.LoadNetwork(cnnNetwork, FLAGS_d, minLatencyConfig);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 5. Create infer requests ------------------------------------------------
        std::vector<InferRequest::Ptr> userSpecifiedInferRequests;
        for (unsigned infReqId = 0; infReqId < FLAGS_nireq; ++infReqId) {
            userSpecifiedInferRequests.push_back(userSpecifiedExecNetwork.CreateInferRequestPtr());
        }

        InferRequest::Ptr minLatencyInferRequest = minLatencyExecNetwork.CreateInferRequestPtr();

        /* it's enough just to set image info input (if used in the model) only once */
        if (!imageInfoInputName.empty()) {
            auto setImgInfoBlob = [&](const InferRequest::Ptr &inferReq) {
                auto blob = inferReq->GetBlob(imageInfoInputName);
                auto data = blob->buffer().as<PrecisionTrait<Precision::FP32>::value_type *>();
                data[0] = static_cast<float>(netInputHeight);  // height
                data[1] = static_cast<float>(netInputWidth);  // width
                data[2] = 1;
            };

            for (unsigned infReqId = 0; infReqId < FLAGS_nireq; ++infReqId) {
                InferRequest::Ptr requestPtr = userSpecifiedInferRequests[infReqId];
                setImgInfoBlob(requestPtr);
            }

            InferRequest::Ptr requestPtr = minLatencyInferRequest;
            setImgInfoBlob(requestPtr);
        }
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 6. Init variables -------------------------------------------------------
        typedef std::chrono::duration<double, std::chrono::milliseconds::period> ms;
        typedef std::chrono::duration<double, std::chrono::seconds::period> sec;
        
        struct RequestResult {
            cv::Mat frame;
            const float* output;
            std::chrono::high_resolution_clock::time_point startTime;
            ms wallclockTime;
            bool isSameMode;
        };

        struct ModeInfo {
           int framesCount = 0;
           double latencySum = 0;
           std::chrono::high_resolution_clock::time_point lastStartTime = std::chrono::high_resolution_clock::now();
           std::chrono::high_resolution_clock::time_point lastEndTime;
        };

        bool isUserSpecifiedMode = true;  // execution always starts in USER_SPECIFIED mode

        auto totalT0 = std::chrono::high_resolution_clock::now();
        auto prevWallclockTime = std::chrono::high_resolution_clock::now();
        double ocvRenderTime = 0;
        double ocvDecodeTime = 0;

        std::queue<InferRequest::Ptr> emptyRequests;
        if (isUserSpecifiedMode) {
            for (const auto& request: userSpecifiedInferRequests) {
                emptyRequests.push(request);
            }
        } else emptyRequests.push(minLatencyInferRequest);

        std::unordered_map<int, RequestResult> completedRequestResults;
        int nextFrameId = 0;
        int nextFrameIdToShow = 0;
        std::exception_ptr callbackException = nullptr;
        std::mutex mutex;
        std::condition_variable condVar;
        std::map<bool, ModeInfo> modeInfo = {{true, ModeInfo()}, {false, ModeInfo()}};

        cv::Size graphSize{static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH) / 4), 60};
        Presenter presenter(FLAGS_u, static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT)) - graphSize.height - 10,
                            graphSize);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 7. Do inference ---------------------------------------------------------
        slog::info << "Start inference " << slog::endl;

        std::cout << "To close the application, press 'CTRL+C' here or switch to the output window and "
                     "press ESC or 'q' key" << std::endl;
        std::cout << "To switch between min_latency/user_specified modes, press TAB key in the output window" 
                  << std::endl;

        while ((cap.isOpened()
                || !completedRequestResults.empty()
                || ((isUserSpecifiedMode && emptyRequests.size() < FLAGS_nireq)
                    || (!isUserSpecifiedMode && emptyRequests.size() < 1)))
               && callbackException == nullptr) {
            if (callbackException) std::rethrow_exception(callbackException);

            RequestResult requestResult;
            {
                std::lock_guard<std::mutex> lock(mutex);

                auto requestResultItr = completedRequestResults.find(nextFrameIdToShow);
                if (requestResultItr != completedRequestResults.end()) {
                    requestResult = requestResultItr->second;
                    completedRequestResults.erase(requestResultItr);
                } else requestResult.output = nullptr;
            }

            if (requestResult.output != nullptr) {
                const float *detections = requestResult.output;

                nextFrameIdToShow++;
                if (requestResult.isSameMode) {
                    modeInfo[isUserSpecifiedMode].framesCount += 1;
                }

                auto t0 = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < maxProposalCount; i++) {
                    float image_id = detections[i * objectSize + 0];
                    if (image_id < 0) {
                        break;
                    }

                    float confidence = detections[i * objectSize + 2];
                    auto label = static_cast<int>(detections[i * objectSize + 1]);
                    float xmin = detections[i * objectSize + 3] * width;
                    float ymin = detections[i * objectSize + 4] * height;
                    float xmax = detections[i * objectSize + 5] * width;
                    float ymax = detections[i * objectSize + 6] * height;

                    if (FLAGS_r) {
                        std::cout << "[" << i << "," << label << "] element, prob = " << confidence <<
                                  "    (" << xmin << "," << ymin << ")-(" << xmax << "," << ymax << ")"
                                  << ((confidence > FLAGS_t) ? " WILL BE RENDERED!" : "") << std::endl;
                    }

                    if (confidence > FLAGS_t) {
                        /** Drawing only objects when > confidence_threshold probability **/
                        std::ostringstream conf;
                        conf << ":" << std::fixed << std::setprecision(3) << confidence;
                        cv::putText(requestResult.frame,
                                    (static_cast<size_t>(label) < labels.size() ?
                                    labels[label] : std::string("label #") + std::to_string(label)) + conf.str(),
                                    cv::Point2f(xmin, ymin - 5), cv::FONT_HERSHEY_COMPLEX_SMALL, 1,
                                    cv::Scalar(0, 0, 255));
                        cv::rectangle(requestResult.frame, cv::Point2f(xmin, ymin), cv::Point2f(xmax, ymax),
                                      cv::Scalar(0, 0, 255));
                    }
                }

                presenter.drawGraphs(requestResult.frame);

                std::ostringstream out;
                out << "OpenCV cap/render time: " << std::fixed << std::setprecision(2)
                    << (ocvDecodeTime + ocvRenderTime) << " ms";
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 25), cv::FONT_HERSHEY_TRIPLEX, 0.6, 
                            cv::Scalar(255, 255, 255), 2); // make text distinguishable from background
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 25), cv::FONT_HERSHEY_TRIPLEX, 0.6, 
                            cv::Scalar(0, 255, 0), 1);
                out.str("");
                out << "Wallclock time " << (isUserSpecifiedMode ? "(USER SPECIFIED):      " : "(MIN LATENCY, "
                       "press Tab): ");
                out << std::fixed << std::setprecision(2) << requestResult.wallclockTime.count()
                    << " ms (" << 1000.f / requestResult.wallclockTime.count() << " fps)";
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 50), cv::FONT_HERSHEY_TRIPLEX, 0.6,
                            cv::Scalar(255, 255, 255), 2); // make text distinguishable from background
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 50), cv::FONT_HERSHEY_TRIPLEX, 0.6,
                            cv::Scalar(0, 0, 255), 1);
                out.str("");
                out << "FPS: " << std::fixed << std::setprecision(2) << modeInfo[isUserSpecifiedMode].framesCount / 
                    std::chrono::duration_cast<sec>(std::chrono::high_resolution_clock::now() -
                                                    modeInfo[isUserSpecifiedMode].lastStartTime).count();
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 75), cv::FONT_HERSHEY_TRIPLEX, 0.6, 
                            cv::Scalar(255, 255, 255), 2); // make text distinguishable from background
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 75), cv::FONT_HERSHEY_TRIPLEX, 0.6, 
                            cv::Scalar(255, 0, 0), 1);
                out.str("");
                modeInfo[isUserSpecifiedMode].latencySum += std::chrono::duration_cast<sec>(
                    std::chrono::high_resolution_clock::now() - requestResult.startTime).count();
                out << "Latency: " << std::fixed << std::setprecision(2) << (modeInfo[isUserSpecifiedMode].latencySum /
                    modeInfo[isUserSpecifiedMode].framesCount) * 1e3 << " ms";
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 100), cv::FONT_HERSHEY_TRIPLEX, 0.6, 
                            cv::Scalar(255, 255, 255), 2); // make text distinguishable from background
                cv::putText(requestResult.frame, out.str(), cv::Point2f(10, 100), cv::FONT_HERSHEY_TRIPLEX, 0.6, 
                            cv::Scalar(255, 0, 255), 1);

                if (!FLAGS_no_show) {
                    cv::imshow("Detection Results", requestResult.frame);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    ocvRenderTime = std::chrono::duration_cast<ms>(t1 - t0).count();
                    
                    const int key = cv::waitKey(1);

                    if (27 == key || 'q' == key || 'Q' == key) {  // Esc
                        break;
                    }
                    else if (9 == key) {  // Tab
                        bool prevMode = isUserSpecifiedMode;
                        isUserSpecifiedMode ^= true;

                        if (isUserSpecifiedMode) {
                            for (const auto& request: userSpecifiedInferRequests) {
                                request->Wait(IInferRequest::WaitMode::RESULT_READY);
                            }
                        } else minLatencyInferRequest->Wait(IInferRequest::WaitMode::RESULT_READY);
                        
                        std::queue<InferRequest::Ptr> emptyQueue;
                        std::swap(emptyRequests, emptyQueue);
                        if (isUserSpecifiedMode) {
                            for (const auto& request: userSpecifiedInferRequests) {
                                emptyRequests.push(request);
                            }
                        } else emptyRequests.push(minLatencyInferRequest);

                        modeInfo[prevMode].lastEndTime = std::chrono::high_resolution_clock::now();
                        modeInfo[isUserSpecifiedMode] = ModeInfo();
                    } else {
                        presenter.handleKey(key);
                    }
                }
            }
            else if (!emptyRequests.empty() && cap.isOpened()) {
                auto startTime = std::chrono::high_resolution_clock::now();
                
                auto t0 = std::chrono::high_resolution_clock::now();
                cv::Mat frame;
                if (!cap.read(frame)) {
                    if (frame.empty()) {
                        if (FLAGS_loop_input) {
                            cap.open((FLAGS_i == "cam") ? 0 : FLAGS_i.c_str());
                        } else cap.release();
                        continue;
                    } else {
                        throw std::logic_error("Failed to get frame from cv::VideoCapture");
                    }
                }

                InferRequest::Ptr request;
                {
                    std::lock_guard<std::mutex> lock(mutex);

                    request = emptyRequests.front();
                    emptyRequests.pop();
                }
                frameToBlob(frame, request, imageInputName);
                auto t1 = std::chrono::high_resolution_clock::now();
                ocvDecodeTime = std::chrono::duration_cast<ms>(t1 - t0).count();
                
                bool frameMode = isUserSpecifiedMode;
                request->SetCompletionCallback([&mutex,
                                                &completedRequestResults,
                                                nextFrameId,
                                                frame,
                                                request,
                                                outputName,
                                                startTime,
                                                &prevWallclockTime,
                                                frameMode,
                                                &isUserSpecifiedMode,
                                                &emptyRequests,
                                                &callbackException,
                                                &condVar] {
                    {
                        std::lock_guard<std::mutex> callbackLock(mutex);
                    
                        try {
                            auto t0 = std::chrono::high_resolution_clock::now();
                            completedRequestResults.insert(
                                std::pair<int, RequestResult>(nextFrameId, RequestResult{
                                    frame, request->GetBlob(outputName)->buffer().as<float*>(),
                                    startTime, std::chrono::duration_cast<ms>(t0 - prevWallclockTime),
                                    frameMode == isUserSpecifiedMode}));
                            prevWallclockTime = t0;
                            
                            if (isUserSpecifiedMode == frameMode) {
                                emptyRequests.push(request);
                            }
                        }
                        catch (...) {
                            if (!callbackException) {
                                callbackException = std::current_exception();
                            }
                        }
                    }
                    condVar.notify_one();
                });

                request->StartAsync();
                nextFrameId++;
            }
            else {
                std::unique_lock<std::mutex> lock(mutex);

                while (callbackException == nullptr && emptyRequests.empty() && completedRequestResults.empty()) {
                    condVar.wait(lock);
                }
            }
        }

        if (isUserSpecifiedMode) {
            for (const auto& request: userSpecifiedInferRequests) {
                request->Wait(IInferRequest::WaitMode::RESULT_READY);
            }
        } else minLatencyInferRequest->Wait(IInferRequest::WaitMode::RESULT_READY);
        // -----------------------------------------------------------------------------------------------------
        
        // --------------------------- 8. Report metrics -------------------------------------------------------
        slog::info << slog::endl << "Metric reports:" << slog::endl;

        auto totalT1 = std::chrono::high_resolution_clock::now();
        ms total = std::chrono::duration_cast<ms>(totalT1 - totalT0);
        std::cout << std::endl << "Total Inference time: " << total.count() << std::endl;

        /** Show performace results **/
        if (FLAGS_pc) {
            if (isUserSpecifiedMode) {
                for (const auto& request: userSpecifiedInferRequests) {
                    printPerformanceCounts(*request, std::cout, getFullDeviceName(ie, FLAGS_d));
                }
            } else printPerformanceCounts(*minLatencyInferRequest, std::cout, getFullDeviceName(ie, FLAGS_d));
        }

        std::chrono::high_resolution_clock::time_point endTime;
        if (modeInfo[true].framesCount) {
            std::cout << std::endl;
            std::cout << "USER_SPECIFIED mode:" << std::endl;
            endTime = (modeInfo[true].lastEndTime.time_since_epoch().count() != 0)
                      ? modeInfo[true].lastEndTime
                      : std::chrono::high_resolution_clock::now();
            std::cout << "FPS: " << std::fixed << std::setprecision(1)
                << modeInfo[true].framesCount / std::chrono::duration_cast<sec>(
                                                    endTime - modeInfo[true].lastStartTime).count() << std::endl;
            std::cout << "Latency: " << std::fixed << std::setprecision(1)
                << ((modeInfo[true].latencySum / modeInfo[true].framesCount) * 1e3) << std::endl;
        }

        if (modeInfo[false].framesCount) {
            std::cout << std::endl;
            std::cout << "MIN_LATENCY mode:" << std::endl;
            endTime = (modeInfo[false].lastEndTime.time_since_epoch().count() != 0)
                      ? modeInfo[false].lastEndTime
                      : std::chrono::high_resolution_clock::now();
            std::cout << "FPS: " << std::fixed << std::setprecision(1)
                << modeInfo[false].framesCount / std::chrono::duration_cast<sec>(
                                                    endTime - modeInfo[false].lastStartTime).count() << std::endl;
            std::cout << "Latency: " << std::fixed << std::setprecision(1)
                << ((modeInfo[false].latencySum / modeInfo[false].framesCount) * 1e3) << std::endl;
        }

        std::cout << std::endl << presenter.reportMeans() << '\n';
        // -----------------------------------------------------------------------------------------------------
    }
    catch (const std::exception& error) {
        std::cerr << "[ ERROR ] " << error.what() << std::endl;
        return 1;
    }
    catch (...) {
        std::cerr << "[ ERROR ] Unknown/internal exception happened." << std::endl;
        return 1;
    }

    slog::info << slog::endl << "The execution has completed successfully" << slog::endl;
    return 0;
}
