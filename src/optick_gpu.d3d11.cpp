// The MIT License(MIT)
//
// Copyright(c) 2019 Vadim Slyusarev
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "optick.config.h"
#if USE_OPTICK
#ifdef OPTICK_ENABLE_GPU_D3D11

#include "optick_common.h"
#include "optick_memory.h"
#include "optick_core.h"
#include "optick_gpu.h"

#include <atomic>
#include <thread>

#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_5.h>


#define OPTICK_CHECK(args) do { HRESULT __hr = args; (void)__hr; OPTICK_ASSERT(__hr == S_OK, "Failed check"); } while(false);

namespace Optick
{
	template <class T> void SafeRelease(T** ppT)
	{
		if (*ppT)
		{
			(*ppT)->Release();
			*ppT = NULL;
		}
	}

	class GPUProfilerD3D11 : public GPUProfiler
	{
		struct Frame
		{
			ID3D11Query* queryDisjoint = nullptr;
			
			Frame()
			{
				Reset();
			}

			void Reset()
			{
			}

			void Shutdown();

			~Frame()
			{
				Shutdown();
			}
		};

		struct NodePayload
		{
			array<Frame, NUM_FRAMES_DELAY> frames;
			std::array<ID3D11Query*, MAX_QUERIES_COUNT> stamps{};
			ID3D11Fence* syncFence = nullptr;

			NodePayload() //: commandQueue(nullptr), queryHeap(nullptr), syncFence(nullptr)
			{}
			~NodePayload();
		};
		vector<NodePayload*> nodePayloads;

		bool isSignalingSupported = false;
		ID3D11Device* device;
		ID3D11DeviceContext* context;

		ID3D11Device5* device5 = nullptr;
		ID3D11DeviceContext4* context4 = nullptr;

		// VSync Stats
		DXGI_FRAME_STATISTICS prevFrameStatistics;

		//void UpdateRange(uint32_t start, uint32_t finish)
		void InitNodeInternal(const char* nodeName, uint32_t nodeIndex);

		void ResolveTimestamps(uint32_t startIndex, uint32_t count);

		void WaitForFrame(uint64_t frameNumber);

	public:
		GPUProfilerD3D11();
		~GPUProfilerD3D11();

		void InitDevice(ID3D11Device* pDevice, ID3D11DeviceContext* pCommandQueues, uint32_t numCommandQueues);

		void QueryTimestamp(ID3D11DeviceContext* context, int64_t* outCpuTimestamp);

		void Flip(IDXGISwapChain* swapChain);


		// Interface implementation
		ClockSynchronization GetClockSynchronization(uint32_t nodeIndex) override;

		void QueryTimestamp(void* inContext, int64_t* outCpuTimestamp) override
		{
			QueryTimestamp(context, outCpuTimestamp);
		}

		void Flip(void* swapChain) override
		{
			Flip(static_cast<IDXGISwapChain*>(swapChain));
		}
	};

	void InitGpuD3D11(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t numQueues)
	{
		GPUProfilerD3D11* gpuProfiler = Memory::New<GPUProfilerD3D11>();
		gpuProfiler->InitDevice(device, context, numQueues);
		Core::Get().InitGPUProfiler(gpuProfiler);
	}

	GPUProfilerD3D11::GPUProfilerD3D11() : device(nullptr)
	{
		prevFrameStatistics = { 0 };
	}

	GPUProfilerD3D11::~GPUProfilerD3D11()
	{
		for (NodePayload* payload : nodePayloads)
			Memory::Delete(payload);
		nodePayloads.clear();

		for (Node* node : nodes)
			Memory::Delete(node);
		nodes.clear();

		SafeRelease(&device5);
		SafeRelease(&context4);
	}

	void GPUProfilerD3D11::InitDevice(ID3D11Device* pDevice, ID3D11DeviceContext* pCommandQueues, uint32_t numCommandQueues)
	{
		device = pDevice;
		context = pCommandQueues;

		device->QueryInterface(&device5);
		context->QueryInterface(&context4);
		isSignalingSupported = device5 && context4;
		
		uint32_t nodeCount = 1; // numCommandQueues; // pDevice->GetNodeCount();

		nodes.resize(nodeCount);
		nodePayloads.resize(nodeCount);

		IDXGIDevice2* dxgiDevice;
		pDevice->QueryInterface(&dxgiDevice);

		IDXGIAdapter* adapter;
		dxgiDevice->GetAdapter(&adapter);
		
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		SafeRelease(&adapter);

		char deviceName[128] = { 0 };
		wcstombs_s(deviceName, desc.Description, OPTICK_ARRAY_SIZE(deviceName) - 1);

		for (uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
			InitNodeInternal(deviceName, nodeIndex);
	}

	void GPUProfilerD3D11::InitNodeInternal(const char* nodeName, uint32_t nodeIndex)
	{
		GPUProfiler::InitNode(nodeName, nodeIndex);

		NodePayload* node = Memory::New<NodePayload>();
		nodePayloads[nodeIndex] = node;

		//D3D12_QUERY_HEAP_DESC queryHeapDesc;
		//queryHeapDesc.Count = MAX_QUERIES_COUNT;
		//queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		//queryHeapDesc.NodeMask = 1u << nodeIndex;
		//OPTICK_CHECK(device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&node->queryHeap)));
		//OPTICK_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&node->syncFence)));

		if (isSignalingSupported)
		{
			device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&node->syncFence));
		}

		for (Frame& frame : node->frames)
		{
			//OPTICK_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.commandAllocator)));
			//OPTICK_CHECK(device->CreateCommandList(1u << nodeIndex, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.commandAllocator, nullptr, IID_PPV_ARGS(&frame.commandList)));
			//OPTICK_CHECK(frame.commandList->Close());
			
		}
	}

	void GPUProfilerD3D11::QueryTimestamp(ID3D11DeviceContext* context, int64_t* outCpuTimestamp)
	{
		if (currentState == STATE_RUNNING)
		{
			uint32_t index = nodes[currentNode]->QueryTimestamp(outCpuTimestamp);

			if (nodePayloads[currentNode]->stamps[index] == nullptr) // Create 8K queries from the start?
			{
				ID3D11Query* timestampQuery;
				D3D11_QUERY_DESC qDesc{
					D3D11_QUERY_TIMESTAMP,
					0
				};
				device->CreateQuery(&qDesc, &timestampQuery);
				nodePayloads[currentNode]->stamps[index] = timestampQuery;
			}
			context->End(nodePayloads[currentNode]->stamps[index]);
		}
	}

	void GPUProfilerD3D11::ResolveTimestamps(uint32_t startIndex, uint32_t count)
	{
		if (count)
		{
			Node* node = nodes[currentNode];
			NodePayload* payload = nodePayloads[currentNode];

			// Convert GPU timestamps => CPU Timestamps
			for (uint32_t index = startIndex; index < startIndex + count; ++index)
			{
				//auto getDataRet = context->GetData(payload->stamps[index], &node->queryGpuTimestamps[index], sizeof(uint64_t), 0) == S_OK;
				//OPTICK_ASSERT(getDataRet, "Timestamps should be ready at this point");

				while (context->GetData(payload->stamps[index], &node->queryGpuTimestamps[index], sizeof(uint64_t), 0) == S_FALSE)
				{}

				*node->queryCpuTimestamps[index] = node->clock.GetCPUTimestamp(node->queryGpuTimestamps[index]);
			}
		}
	}

	void GPUProfilerD3D11::WaitForFrame(uint64_t frameNumberToWait)
	{
		OPTICK_EVENT();

		NodePayload* payload = nodePayloads[currentNode];
		if (isSignalingSupported)
		{
			while (frameNumberToWait > payload->syncFence->GetCompletedValue())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		else {
			OPTICK_ASSERT(false, "WaitForFrame with Disjoint is not implemented yet");
			//uint32_t frameIndexToWait = frameNumberToWait % NUM_FRAMES_DELAY;
			//D3D11_QUERY_DATA_TIMESTAMP_DISJOINT tsDisjoint;
			//while (context->GetData(payload->frames[frameIndexToWait].queryDisjoint, &tsDisjoint, sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT), 0) == S_FALSE)
			//{
			//	std::this_thread::sleep_for(std::chrono::milliseconds(1));
			//}
		}
	}

	void GPUProfilerD3D11::Flip(IDXGISwapChain* swapChain)
	{
		OPTICK_CATEGORY("GPUProfilerD3D11::Flip", Category::Debug);

		std::lock_guard<std::recursive_mutex> lock(updateLock);

		if (currentState == STATE_STARTING)
			currentState = STATE_RUNNING;

		if (currentState == STATE_RUNNING)
		{
			Node& node = *nodes[currentNode];
			NodePayload& payload = *nodePayloads[currentNode];

			uint32_t currentFrameIndex = frameNumber % NUM_FRAMES_DELAY;
			uint32_t nextFrameIndex = (frameNumber + 1) % NUM_FRAMES_DELAY;

			QueryFrame& currentFrame = node.queryGpuframes[currentFrameIndex];
			QueryFrame& nextFrame = node.queryGpuframes[nextFrameIndex];

			//ID3D12GraphicsCommandList* commandList = payload.frames[currentFrameIndex].commandList;
			//ID3D12CommandAllocator* commandAllocator = payload.frames[currentFrameIndex].commandAllocator;
			//commandAllocator->Reset();
			//commandList->Reset(commandAllocator, nullptr);

			if (EventData* frameEvent = currentFrame.frameEvent)
				QueryTimestamp(context, &frameEvent->finish);

			if (!isSignalingSupported)
			{
				// End disjoint query for this frame

				// Begin disjoint query for next frame
			}

			// Generate GPU Frame event for the next frame
			EventData& event = AddFrameEvent();
			QueryTimestamp(context, &event.start);
			QueryTimestamp(context, &AddFrameTag().timestamp);
			nextFrame.frameEvent = &event;

			uint32_t queryBegin = currentFrame.queryIndexStart;
			uint32_t queryEnd = node.queryIndex;

			if (queryBegin != (uint32_t)-1)
			{
				OPTICK_ASSERT(queryEnd - queryBegin <= MAX_QUERIES_COUNT, "Too many queries in one frame? Increase GPUProfiler::MAX_QUERIES_COUNT to fix the problem!");
				currentFrame.queryIndexCount = queryEnd - queryBegin;

				uint32_t startIndex = queryBegin % MAX_QUERIES_COUNT;
				uint32_t finishIndex = queryEnd % MAX_QUERIES_COUNT;

				//if (startIndex < finishIndex)
				//{
				//	commandList->ResolveQueryData(payload.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, startIndex, queryEnd - queryBegin, queryBuffer, startIndex * sizeof(int64_t));
				//}
				//else
				//{
				//	commandList->ResolveQueryData(payload.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, startIndex, MAX_QUERIES_COUNT - startIndex, queryBuffer, startIndex * sizeof(int64_t));
				//	commandList->ResolveQueryData(payload.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, finishIndex, queryBuffer, 0);
				//}
			}

			//commandList->Close();
			//payload.commandQueue->ExecuteCommandLists(1, (ID3D12CommandList*const*)&commandList);
			
			context4->Signal(payload.syncFence, frameNumber);

			// Preparing Next Frame
			// Try resolve timestamps for the current frame
			if (frameNumber >= NUM_FRAMES_DELAY && nextFrame.queryIndexCount)
			{
				WaitForFrame(frameNumber + 1 - NUM_FRAMES_DELAY);

				uint32_t resolveStart = nextFrame.queryIndexStart % MAX_QUERIES_COUNT;
				uint32_t resolveFinish = resolveStart + nextFrame.queryIndexCount;
				ResolveTimestamps(resolveStart, std::min<uint32_t>(resolveFinish, MAX_QUERIES_COUNT) - resolveStart);
				if (resolveFinish > MAX_QUERIES_COUNT)
					ResolveTimestamps(0, resolveFinish - MAX_QUERIES_COUNT);
			}

			nextFrame.queryIndexStart = queryEnd;
			nextFrame.queryIndexCount = 0;

			// Process VSync
			DXGI_FRAME_STATISTICS currentFrameStatistics = { 0 };
			HRESULT result = swapChain->GetFrameStatistics(&currentFrameStatistics);
			if ((result == S_OK) && (prevFrameStatistics.PresentCount + 1 == currentFrameStatistics.PresentCount))
			{
				EventData& data = AddVSyncEvent();
				data.start = prevFrameStatistics.SyncQPCTime.QuadPart;
				data.finish = currentFrameStatistics.SyncQPCTime.QuadPart;
			}
			prevFrameStatistics = currentFrameStatistics;
		}

		++frameNumber;
	}

	GPUProfiler::ClockSynchronization GPUProfilerD3D11::GetClockSynchronization(uint32_t nodeIndex)
	{
		ClockSynchronization clock;
		clock.frequencyCPU = GetHighPrecisionFrequency();

		D3D11_QUERY_DESC disDesc {
			D3D11_QUERY_TIMESTAMP_DISJOINT,
			0
		};
		D3D11_QUERY_DESC timeDesc{
			D3D11_QUERY_TIMESTAMP,
			0
		};
		ID3D11Query* disQuery = nullptr;
		ID3D11Query* timeQuery = nullptr;
		OPTICK_CHECK( device->CreateQuery(&disDesc, &disQuery) );
		OPTICK_CHECK( device->CreateQuery(&timeDesc, &timeQuery) );
		context->Begin(disQuery);
		context->End(timeQuery);
		context->End(disQuery);

		context->Flush();

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT tsDisjoint;
		while (context->GetData(disQuery, &tsDisjoint, sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT), 0) == S_FALSE)
		{ }

		LARGE_INTEGER cpuCounter;
		bool perfCount = QueryPerformanceCounter(&cpuCounter);
		OPTICK_ASSERT(perfCount, "Cant't get CPU performance counter");
		clock.timestampCPU = cpuCounter.QuadPart;

		OPTICK_ASSERT(tsDisjoint.Disjoint == false, "Query is disjoint");

		uint64_t gpuTime = 0;
		auto getDataRet = context->GetData(timeQuery, &gpuTime, sizeof(uint64_t), 0) == S_OK;
		OPTICK_ASSERT(getDataRet, "Can't get data from timeQuery");

		clock.timestampGPU = static_cast<int64_t>(gpuTime); // unsigned to signed, what could go wrong? 
		clock.frequencyGPU = static_cast<int64_t>(tsDisjoint.Frequency);

		SafeRelease(&disQuery);
		SafeRelease(&timeQuery);

		return clock;
	}

	GPUProfilerD3D11::NodePayload::~NodePayload()
	{
		SafeRelease(&syncFence);

		for (int i = 0; i < stamps.size(); ++i)
		{
			SafeRelease(&stamps[i]);
		}
	}

	void GPUProfilerD3D11::Frame::Shutdown()
	{
		SafeRelease(&queryDisjoint);
	}
}

#else
#include "optick_common.h"

namespace Optick
{
	void InitGpuD3D11(ID3D11Device* /*device*/, ID3D11DeviceContext* /*cmdQueues*/, uint32_t /*numQueues*/)
	{
		OPTICK_FAILED("OPTICK_ENABLE_GPU_D3D11 is disabled! Can't initialize GPU Profiler!");
	}
}

#endif //OPTICK_ENABLE_GPU_D3D12
#endif //USE_OPTICK