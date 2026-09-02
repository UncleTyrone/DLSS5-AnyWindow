#include "../Magpie.Core/pch.h"
#include "../Magpie.Core/DeviceResources.h"
#include "../Magpie.Core/DLSSNRFilter.h"
#include "../Magpie.Core/DLSSZeroMVUpscaler.h"
#include "../Magpie.Core/ZeroFrameGuidanceProvider.h"
#include "../Shared/Logger.h"

#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace Magpie;

namespace {

enum class Pipeline : int {
	NeuralRender = 0,
	SuperResolution = 1,
	SuperResolutionThenNeuralRender = 2
};

bool ReadFrame(std::vector<uint8_t>& frame) {
	size_t read = 0;
	while (read < frame.size()) {
		const size_t n = std::fread(frame.data() + read, 1, frame.size() - read, stdin);
		if (!n) {
			if (read == 0 && std::feof(stdin)) return false;
			throw std::runtime_error("partial input frame");
		}
		read += n;
	}
	return true;
}

void WriteFrame(const std::vector<uint8_t>& frame) {
	size_t written = 0;
	while (written < frame.size()) {
		const size_t n = std::fwrite(
			frame.data() + written, 1, frame.size() - written, stdout);
		if (!n) throw std::runtime_error("output pipe closed");
		written += n;
	}
}

winrt::com_ptr<ID3D11Texture2D> CreateTexture(
	ID3D11Device5* device,
	uint32_t width,
	uint32_t height,
	D3D11_USAGE usage,
	UINT bindFlags,
	UINT cpuAccessFlags
) {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = usage;
	desc.BindFlags = bindFlags;
	desc.CPUAccessFlags = cpuAccessFlags;
	winrt::com_ptr<ID3D11Texture2D> result;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, result.put()))) return nullptr;
	return result;
}

int ParseInt(const wchar_t* value, int minimum, int maximum) {
	const long parsed = std::wcstol(value, nullptr, 10);
	if (parsed < minimum || parsed > maximum) {
		throw std::runtime_error("integer argument out of range");
	}
	return static_cast<int>(parsed);
}

float ParseFloat(const wchar_t* value) {
	const float parsed = std::wcstof(value, nullptr);
	if (!(parsed >= 0.0f && parsed <= 1.0f)) {
		throw std::runtime_error("float argument out of range");
	}
	return parsed;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	if (argc != 11 && argc != 12 && argc != 13) {
		std::wcerr <<
			L"Usage: DLSSNROffscreen inputWidth inputHeight outputWidth outputHeight "
			L"pipeline style intensity tone structure autoMask [passes] [antiFlicker]\n";
		return 2;
	}

	try {
		const uint32_t inputWidth = static_cast<uint32_t>(ParseInt(argv[1], 16, 16384));
		const uint32_t inputHeight = static_cast<uint32_t>(ParseInt(argv[2], 16, 16384));
		const uint32_t outputWidth = static_cast<uint32_t>(ParseInt(argv[3], 16, 16384));
		const uint32_t outputHeight = static_cast<uint32_t>(ParseInt(argv[4], 16, 16384));
		const Pipeline pipeline = static_cast<Pipeline>(ParseInt(argv[5], 0, 2));
		const bool useSuperResolution = pipeline != Pipeline::NeuralRender;
		const bool useNeuralRender = pipeline != Pipeline::SuperResolution;

		if (!useSuperResolution &&
			(inputWidth != outputWidth || inputHeight != outputHeight)) {
			throw std::runtime_error("neural-render-only pipeline requires equal dimensions");
		}
		if (useSuperResolution &&
			(inputWidth > outputWidth || inputHeight > outputHeight)) {
			throw std::runtime_error("DLSS Super Resolution only supports upscaling");
		}

		DLSSNRSettings nrSettings{
			.style = ParseInt(argv[6], 0, 2),
			.intensity = ParseFloat(argv[7]),
			.localToneStrength = ParseFloat(argv[8]),
			.localStructureStrength = ParseFloat(argv[9]),
			.useAutoMask = ParseInt(argv[10], 0, 1) != 0,
			.guidanceMode = 1,
			.depthInferenceInterval = 4,
			.passes = argc >= 12 ?
				static_cast<uint32_t>(ParseInt(argv[11], 1, 4)) : 1u,
			.antiFlicker = argc == 13 && ParseInt(argv[12], 0, 1) != 0
		};

		_setmode(_fileno(stdin), _O_BINARY);
		_setmode(_fileno(stdout), _O_BINARY);
		Logger::Get().Initialize(
			spdlog::level::info, L"dlssnr-offscreen.log", 2 * 1024 * 1024, 1);

		DeviceResources resources;
		if (!resources.InitializeOffscreen()) {
			throw std::runtime_error("D3D initialization failed");
		}
		ID3D11Device5* device = resources.GetD3DDevice();
		ID3D11DeviceContext4* context = resources.GetD3DDC();

		auto input = CreateTexture(
			device, inputWidth, inputHeight, D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE, 0);
		auto srOutput = useSuperResolution ? CreateTexture(
			device, outputWidth, outputHeight, D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0) : nullptr;
		auto nrOutput = useNeuralRender ? CreateTexture(
			device, outputWidth, outputHeight, D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0) : nullptr;
		auto readback = CreateTexture(
			device, outputWidth, outputHeight, D3D11_USAGE_STAGING,
			0, D3D11_CPU_ACCESS_READ);
		if (!input || (useSuperResolution && !srOutput) ||
			(useNeuralRender && !nrOutput) || !readback) {
			throw std::runtime_error("texture creation failed");
		}

		DLSSZeroMVUpscaler srFilter;
		if (useSuperResolution) {
			const DLSSSRSettings srSettings{
				.enableJitter = false,
				.useMotionVectors = false,
				.useEstimatedDepth = false
			};
			if (!srFilter.Initialize(
				resources, input.get(), srOutput.get(), srSettings)) {
				throw std::runtime_error("DLSS Super Resolution initialization failed");
			}
		}

		ZeroFrameGuidanceResources zeroResources;
		ZeroDepthProvider zeroDepth(zeroResources);
		ZeroMotionVectorProvider zeroMotion(zeroResources);
		const FrameGuidanceExtent outputExtent{ outputWidth, outputHeight };
		if (useNeuralRender &&
			(!zeroDepth.Initialize(resources, outputExtent) ||
			 !zeroMotion.Initialize(resources, outputExtent))) {
			throw std::runtime_error("zero guidance initialization failed");
		}

		DLSSNRFilter nrFilter;
		ID3D11Texture2D* nrInput = useSuperResolution ? srOutput.get() : input.get();
		if (useNeuralRender &&
			!nrFilter.Initialize(resources, nrInput, nrOutput.get(), nrSettings)) {
			throw std::runtime_error("DLSSNR Feature 18 initialization failed");
		}

		const size_t inputRowBytes = static_cast<size_t>(inputWidth) * 4;
		const size_t inputFrameBytes = inputRowBytes * inputHeight;
		const size_t outputRowBytes = static_cast<size_t>(outputWidth) * 4;
		const size_t outputFrameBytes = outputRowBytes * outputHeight;
		std::vector<uint8_t> source(inputFrameBytes);
		std::vector<uint8_t> result(outputFrameBytes);
		FrameGuidanceView emptyGuidance;
		uint64_t frameId = 0;

		while (ReadFrame(source)) {
			++frameId;
			context->UpdateSubresource(input.get(), 0, nullptr, source.data(),
				static_cast<UINT>(inputRowBytes), 0);

			if (useSuperResolution) {
				const NativeEffectDrawContext srContext{
					.input = input.get(),
					.output = srOutput.get(),
					.frameId = frameId,
					.frameGuidance = emptyGuidance,
					.zeroFrameGuidance = emptyGuidance
				};
				if (!srFilter.Draw(srContext)) {
					throw std::runtime_error("DLSS Super Resolution evaluation failed");
				}
			}

			if (useNeuralRender) {
				FrameGuidanceFrame frame{
					.color = nrInput,
					.frameId = frameId,
					.sourceExtent = outputExtent,
					.validRegion = FrameGuidanceRegion::Full(outputExtent)
				};
				DepthProviderOutput depth;
				MotionVectorProviderOutput motion;
				if (!zeroDepth.BeginFrame(frame, depth) ||
					!zeroMotion.BeginFrame(frame, motion)) {
					throw std::runtime_error("zero guidance frame failed");
				}
				FrameGuidanceView zero;
				zero.depth = depth.depth;
				zero.motion = motion.motion;
				zero.confidence = motion.confidence;
				zero.requiresHistoryReset =
					zero.depth.metadata.requiresHistoryReset ||
					zero.motion.metadata.requiresHistoryReset;

				const NativeEffectDrawContext nrContext{
					.input = nrInput,
					.output = nrOutput.get(),
					.frameId = frameId,
					.frameGuidance = zero,
					.zeroFrameGuidance = zero
				};
				if (!nrFilter.Draw(nrContext)) {
					throw std::runtime_error("DLSSNR evaluation failed");
				}
			}

			ID3D11Texture2D* finalTexture = useNeuralRender ? nrOutput.get() : srOutput.get();
			context->CopyResource(readback.get(), finalTexture);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				throw std::runtime_error("GPU readback failed");
			}
			for (uint32_t y = 0; y < outputHeight; ++y) {
				std::memcpy(result.data() + static_cast<size_t>(y) * outputRowBytes,
					static_cast<const uint8_t*>(mapped.pData) +
					static_cast<size_t>(y) * mapped.RowPitch, outputRowBytes);
			}
			context->Unmap(readback.get(), 0);
			WriteFrame(result);
			std::fflush(stdout);
			if (frameId % 120 == 0) {
				std::cerr << "DLSS_OFFSCREEN frames=" << frameId << "\n";
			}
		}

		std::fflush(stdout);
		Logger::Get().Info(fmt::format(
			"DLSS_OFFSCREEN complete frames={} input={}x{} output={}x{} pipeline={}",
			frameId, inputWidth, inputHeight, outputWidth, outputHeight,
			static_cast<int>(pipeline)));
		Logger::Get().Flush();
		std::cerr << "DLSS_OFFSCREEN complete frames=" << frameId << "\n";
		return 0;
	} catch (const std::exception& error) {
		Logger::Get().Error(error.what());
		Logger::Get().Flush();
		std::cerr << "DLSS_OFFSCREEN error=" << error.what() << "\n";
		return 1;
	}
}
