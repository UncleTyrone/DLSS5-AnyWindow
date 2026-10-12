#pragma once
#include <cstdint>
#include <d3d12.h>

using NvU32 = uint32_t;
using NVDX_ObjectHandle = void*;

enum NvAPI_Status {
	NVAPI_OK = 0,
	NVAPI_ERROR = -1
};

struct NVAPI_DIM3 {
	NvU32 x;
	NvU32 y;
	NvU32 z;
};

struct NVAPI_CU_KERNEL_LAUNCH_PARAMS {
	NVDX_ObjectHandle hFunction;
	NVAPI_DIM3 gridDim;
	NVAPI_DIM3 blockDim;
	NvU32 dynSharedMemBytes;
	const void* pParams;
	NvU32 paramSize;
};

static_assert(sizeof(NVAPI_CU_KERNEL_LAUNCH_PARAMS) == 56);

NvAPI_Status __cdecl NvAPI_D3D12_CreateCuModule(ID3D12Device*, const void*, NvU32, NVDX_ObjectHandle*);
NvAPI_Status __cdecl NvAPI_D3D12_CreateCuFunction(ID3D12Device*, NVDX_ObjectHandle, const char*, NVDX_ObjectHandle*);
NvAPI_Status __cdecl NvAPI_D3D12_LaunchCuKernelChain(ID3D12GraphicsCommandList*, const NVAPI_CU_KERNEL_LAUNCH_PARAMS*, NvU32);
NvAPI_Status __cdecl NvAPI_D3D12_DestroyCuModule(ID3D12Device*, NVDX_ObjectHandle);
NvAPI_Status __cdecl NvAPI_D3D12_DestroyCuFunction(ID3D12Device*, NVDX_ObjectHandle);
