/**********************************************************************
Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/

#include <windows.h>
#include <atlbase.h>
#include <conio.h>

#include <d3d12.h>
#include <d3dx12/d3dx12.h>
#include <dxcapi.h>
#include <dxgi1_6.h>

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 711; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = u8".\\"; }

#define UAV_SIZE 1024

ID3D12Device9* InitializeDirectX();
void ShutdownDirectX();
bool EnsureWorkGraphsSupported(CComPtr<ID3D12Device9> pDevice);
ID3DBlob* CompileGWGLibrary();
ID3D12RootSignature* CreateGlobalRootSignature(CComPtr<ID3D12Device9> pDevice);
ID3D12StateObject* CreateGWGStateObject(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12RootSignature> pGlobalRootSignature, CComPtr<ID3DBlob> pGwgLibrary);
D3D12_SET_PROGRAM_DESC PrepareWorkGraph(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12StateObject> pStateObject);
bool DispatchWorkGraphAndReadResults(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12RootSignature> pGlobalRootSignature, D3D12_SET_PROGRAM_DESC SetProgramDesc, char* pResult);

int main()
{
	try
	{
		CComPtr<ID3D12Device9> pDevice = InitializeDirectX();
		if (EnsureWorkGraphsSupported(pDevice))
		{
			CComPtr<ID3DBlob> pGwgLibrary = CompileGWGLibrary();

			CComPtr<ID3D12RootSignature> pGlobalRootSignature = CreateGlobalRootSignature(pDevice);

			CComPtr<ID3D12StateObject> pStateObject = CreateGWGStateObject(pDevice, pGlobalRootSignature, pGwgLibrary);
			D3D12_SET_PROGRAM_DESC SetProgramDesc = PrepareWorkGraph(pDevice, pStateObject);

			char result[UAV_SIZE / sizeof(char)];
			if (DispatchWorkGraphAndReadResults(pDevice, pGlobalRootSignature, SetProgramDesc, result))
			{
				printf("SUCCESS: Output was \"%s\"\nPress any key to terminate...\n", result);
				_getch();
			}
		}
	}
	catch (...)	{ }
	
	ShutdownDirectX();
	return 0;
}


#define ERROR_QUIT(value, ...) if(!(value)) { printf("ERROR: "); printf(__VA_ARGS__); printf("\nPress any key to terminate...\n"); _getch(); throw 0; }


static HMODULE sDxCompilerDLL = nullptr;
static const wchar_t* kProgramName = L"Hello World";

// function GetHardwareAdapter() copy-pasted from the publicly distributed sample provided at: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
void GetHardwareAdapter(IDXGIFactory4* pFactory, IDXGIAdapter1** ppAdapter)
{
	*ppAdapter = nullptr;
	for (UINT adapterIndex = 0; ; ++adapterIndex)
	{
		IDXGIAdapter1* pAdapter = nullptr;
		if (DXGI_ERROR_NOT_FOUND == pFactory->EnumAdapters1(adapterIndex, &pAdapter))
		{
			// No more adapters to enumerate.
			break;
		}

		// Check to see if the adapter supports Direct3D 12, but don't create the
		// actual device yet.
		if (SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
		{
			*ppAdapter = pAdapter;
			return;
		}
		pAdapter->Release();
	}
}

ID3D12Device9* InitializeDirectX()
{
	ID3D12Device9* pDevice = nullptr;

	UUID GPUWorkGraphExperimentalFeatures[2] = { D3D12ExperimentalShaderModels,D3D12StateObjectsExperiment };
	HRESULT hr = D3D12EnableExperimentalFeatures(_countof(GPUWorkGraphExperimentalFeatures), GPUWorkGraphExperimentalFeatures, nullptr, nullptr);
	
	CComPtr<IDXGIFactory4> pFactory;
	if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&pFactory))))
	{
		CComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(pFactory, &hardwareAdapter);
		D3D12CreateDevice(hardwareAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice));
	}

	sDxCompilerDLL = LoadLibrary(L"dxcompiler.dll");

	ERROR_QUIT(SUCCEEDED(hr) && pDevice && sDxCompilerDLL, "Failed to initialize DirectX. Ensure Developer Mode is enabled.");
	return pDevice;
}

void ShutdownDirectX()
{
	if (sDxCompilerDLL)
	{
		FreeLibrary(sDxCompilerDLL);
		sDxCompilerDLL = nullptr;
	}
	// most entities are automatically cleaned up courtesy of CComPtr
}

bool EnsureWorkGraphsSupported(CComPtr<ID3D12Device9> pDevice)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS_EXPERIMENTAL Options = {};
	pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL, &Options, sizeof(Options));
	ERROR_QUIT(Options.WorkGraphsTier != D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED, "Failed to ensure work graphs were supported. Check driver and graphics card.");

	return (Options.WorkGraphsTier != D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED);
}

ID3DBlob* CompileGWGLibrary()
{
	static const char kSourceCode[] =
		"RWByteAddressBuffer Output : register(u0);"
		""
		"[Shader(\"node\")]"
		"[NodeLaunch(\"Broadcasting\")]"
		"[NodeDispatchGrid(1, 1, 1)]"
		"[NumThreads(1, 1, 1)]"
		"void BroadcastNode()"
		"{"
		"    Output.Store3(0, uint3(0x6C6C6548, 0x6F57206F, 0x00646C72));"
		"}";

	ID3DBlob* pGwgLibrary = nullptr;
	if (sDxCompilerDLL)
	{
		DxcCreateInstanceProc pDxcCreateInstance;
		pDxcCreateInstance = (DxcCreateInstanceProc)GetProcAddress(sDxCompilerDLL, "DxcCreateInstance");

		if (pDxcCreateInstance)
		{
			CComPtr<IDxcUtils> pUtils;
			CComPtr<IDxcCompiler> pCompiler;
			CComPtr<IDxcBlobEncoding> pSource;
			CComPtr<IDxcOperationResult> pOperationResult;

			if (SUCCEEDED(pDxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils))) && SUCCEEDED(pDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler))))
			{
				if (SUCCEEDED(pUtils->CreateBlob(kSourceCode, sizeof(kSourceCode), 0, &pSource)))
				{
					if (SUCCEEDED(pCompiler->Compile(pSource, nullptr, nullptr, L"lib_6_8", nullptr, 0, nullptr, 0, nullptr, &pOperationResult)))
					{
						HRESULT hr;
						pOperationResult->GetStatus(&hr);
						if (SUCCEEDED(hr))
						{
							pOperationResult->GetResult((IDxcBlob**)&pGwgLibrary);
						}
					}
				}
			}
		}
	}

	ERROR_QUIT(pGwgLibrary, "Failed to compile GWG Library.");
	return pGwgLibrary;
}

ID3D12RootSignature* CreateGlobalRootSignature(CComPtr<ID3D12Device9> pDevice)
{
	ID3D12RootSignature* pRootSignature = nullptr;

	CD3DX12_ROOT_PARAMETER RootSignatureUAV;
	RootSignatureUAV.InitAsUnorderedAccessView(0, 0);

	CD3DX12_ROOT_SIGNATURE_DESC Desc(1, &RootSignatureUAV, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
	CComPtr<ID3DBlob> pSerialized;

	HRESULT hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &pSerialized, NULL);
	if (SUCCEEDED(hr))
	{
		hr = pDevice->CreateRootSignature(0, pSerialized->GetBufferPointer(), pSerialized->GetBufferSize(), IID_PPV_ARGS(&pRootSignature));
	}

	ERROR_QUIT(SUCCEEDED(hr), "Failed to create global root signature.");
	return pRootSignature;
}

ID3D12StateObject* CreateGWGStateObject(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12RootSignature> pGlobalRootSignature, CComPtr<ID3DBlob> pGwgLibrary)
{
	ID3D12StateObject* pStateObject = nullptr;
	CD3DX12_STATE_OBJECT_DESC Desc(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);

	CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* pGlobalRootSignatureDesc = Desc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
	pGlobalRootSignatureDesc->SetRootSignature(pGlobalRootSignature);

	CD3DX12_DXIL_LIBRARY_SUBOBJECT* LibraryDesc = Desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
	CD3DX12_SHADER_BYTECODE gwgLibraryCode(pGwgLibrary);
	LibraryDesc->SetDXILLibrary(&gwgLibraryCode);

	CD3DX12_WORK_GRAPH_SUBOBJECT* WorkGraphDesc = Desc.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
	WorkGraphDesc->IncludeAllAvailableNodes();
	WorkGraphDesc->SetProgramName(kProgramName);

	HRESULT hr = pDevice->CreateStateObject(Desc, IID_PPV_ARGS(&pStateObject));
	ERROR_QUIT(SUCCEEDED(hr) && pStateObject, "Failed to create Work Graph State Object.");

	return pStateObject;
}

inline ID3D12Resource* AllocateBuffer(CComPtr<ID3D12Device9> pDevice, UINT64 Size, D3D12_RESOURCE_FLAGS ResourceFlags, D3D12_HEAP_TYPE HeapType)
{
	ID3D12Resource* pResource;

	CD3DX12_HEAP_PROPERTIES HeapProperties(HeapType);
	CD3DX12_RESOURCE_DESC ResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(Size, ResourceFlags);
	HRESULT hr = pDevice->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&pResource));
	ERROR_QUIT(SUCCEEDED(hr), "Failed to allocate buffer.");

	return pResource;
}

D3D12_SET_PROGRAM_DESC PrepareWorkGraph(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12StateObject> pStateObject)
{
	CComPtr<ID3D12Resource> pBackingMemoryResource = nullptr;

	CComPtr<ID3D12StateObjectProperties1> pStateObjectProperties;	
	CComPtr<ID3D12WorkGraphProperties> pWorkGraphProperties;
	pStateObjectProperties = pStateObject;
	pWorkGraphProperties = pStateObject;

	UINT WGIndex = pWorkGraphProperties->GetWorkGraphIndex(kProgramName);
	D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS MemoryRequirements = {};
	pWorkGraphProperties->GetWorkGraphMemoryRequirements(WGIndex, &MemoryRequirements);
	if (MemoryRequirements.MaxSizeInBytes > 0)
	{
		pBackingMemoryResource = AllocateBuffer(pDevice, MemoryRequirements.MaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT);
	}

	UINT test = pWorkGraphProperties->GetNumEntrypoints(WGIndex);

	D3D12_SET_PROGRAM_DESC Desc = {};
	Desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
	Desc.WorkGraph.ProgramIdentifier = pStateObjectProperties->GetProgramIdentifier(kProgramName);
	Desc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
	if (pBackingMemoryResource)
	{
		Desc.WorkGraph.BackingMemory = { pBackingMemoryResource->GetGPUVirtualAddress(), MemoryRequirements.MaxSizeInBytes };
	}

	return Desc;
}

inline bool RunCommandListAndWait(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12CommandQueue> pCommandQueue, CComPtr<ID3D12CommandAllocator> pCommandAllocator, CComPtr<ID3D12GraphicsCommandListExperimental> pCommandList, CComPtr<ID3D12Fence> pFence)
{
	if (SUCCEEDED(pCommandList->Close()))
	{
		pCommandQueue->ExecuteCommandLists(1, CommandListCast(&pCommandList.p));
		pCommandQueue->Signal(pFence, 1);

		HANDLE hCommandListFinished = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (hCommandListFinished)
		{
			pFence->SetEventOnCompletion(1, hCommandListFinished);
			DWORD waitResult = WaitForSingleObject(hCommandListFinished, INFINITE);
			CloseHandle(hCommandListFinished);

			if (waitResult == WAIT_OBJECT_0 && SUCCEEDED(pDevice->GetDeviceRemovedReason()))
			{
				pCommandAllocator->Reset();
				pCommandList->Reset(pCommandAllocator, nullptr);
				return true;
			}
		}
	}

	return false;
}

bool DispatchWorkGraphAndReadResults(CComPtr<ID3D12Device9> pDevice, CComPtr<ID3D12RootSignature> pGlobalRootSignature, D3D12_SET_PROGRAM_DESC SetProgramDesc, char* pResult)
{
	CComPtr<ID3D12CommandQueue> pCommandQueue;
	CComPtr<ID3D12CommandAllocator> pCommandAllocator;
	CComPtr<ID3D12GraphicsCommandListExperimental> pCommandList;
	CComPtr<ID3D12Fence> pFence;

	D3D12_COMMAND_QUEUE_DESC CommandQueueDesc = {};
	CommandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
	CommandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	pDevice->CreateCommandQueue(&CommandQueueDesc, IID_PPV_ARGS(&pCommandQueue));

	pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pCommandAllocator));
	pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pCommandAllocator, nullptr, IID_PPV_ARGS(&pCommandList));
	pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence));

	if (pCommandQueue && pCommandAllocator && pCommandList && pFence)
	{
		CComPtr<ID3D12Resource> pUAVBuffer = AllocateBuffer(pDevice, UAV_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT);
		CComPtr<ID3D12Resource> pReadbackBuffer = AllocateBuffer(pDevice, UAV_SIZE, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_READBACK);

		// dispatch work graph
		D3D12_DISPATCH_GRAPH_DESC DispatchGraphDesc = {};
		DispatchGraphDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
		DispatchGraphDesc.NodeCPUInput = { };
		DispatchGraphDesc.NodeCPUInput.EntrypointIndex = 0;
		DispatchGraphDesc.NodeCPUInput.NumRecords = 1;

		pCommandList->SetComputeRootSignature(pGlobalRootSignature);
		pCommandList->SetComputeRootUnorderedAccessView(0, pUAVBuffer->GetGPUVirtualAddress());
		pCommandList->SetProgram(&SetProgramDesc);
		pCommandList->DispatchGraph(&DispatchGraphDesc);

		// read results
		D3D12_RESOURCE_BARRIER Barrier = {};
		Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		Barrier.Transition.pResource = pUAVBuffer;
		Barrier.Transition.Subresource = 0;
		Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

		pCommandList->ResourceBarrier(1, &Barrier);
		pCommandList->CopyResource(pReadbackBuffer, pUAVBuffer);

		if (RunCommandListAndWait(pDevice, pCommandQueue, pCommandAllocator, pCommandList, pFence))
		{
			char* pOutput;
			D3D12_RANGE range{ 0, UAV_SIZE };
			if (SUCCEEDED(pReadbackBuffer->Map(0, &range, (void**)&pOutput)))
			{
				memcpy(pResult, pOutput, UAV_SIZE);
				pReadbackBuffer->Unmap(0, nullptr);
				return true;
			}
		}
	}
	
	ERROR_QUIT(true, "Failed to dispatch work graph and read results.");
	return false;
}