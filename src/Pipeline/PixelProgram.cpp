// Copyright 2016 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "PixelProgram.hpp"

#include "Constants.hpp"
#include "SamplerCore.hpp"
#include "Device/Primitive.hpp"
#include "Device/Renderer.hpp"

namespace sw {

PixelProgram::PixelProgram(
    const PixelProcessor::State &state,
    const vk::PipelineLayout *pipelineLayout,
    const SpirvShader *spirvShader,
    const vk::DescriptorSet::Bindings &descriptorSets)
    : PixelRoutine(state, pipelineLayout, spirvShader, descriptorSets)
{
}

// Union all cMask and return it as 4 booleans
Int4 PixelProgram::maskAny(Int cMask[4], const SampleSet &samples)
{
	// See if at least 1 sample is used
	Int maskUnion = 0;
	for(unsigned int q : samples)
	{
		maskUnion |= cMask[q];
	}

	// Convert to 4 booleans
	Int4 laneBits = Int4(1, 2, 4, 8);
	Int4 laneShiftsToMSB = Int4(31, 30, 29, 28);
	Int4 mask(maskUnion);
	mask = ((mask & laneBits) << laneShiftsToMSB) >> Int4(31);
	return mask;
}

// Union all cMask/sMask/zMask and return it as 4 booleans
Int4 PixelProgram::maskAny(Int cMask[4], Int sMask[4], Int zMask[4], const SampleSet &samples)
{
	// See if at least 1 sample is used
	Int maskUnion = 0;
	for(unsigned int q : samples)
	{
		maskUnion |= (cMask[q] & sMask[q] & zMask[q]);
	}

	// Convert to 4 booleans
	Int4 laneBits = Int4(1, 2, 4, 8);
	Int4 laneShiftsToMSB = Int4(31, 30, 29, 28);
	Int4 mask(maskUnion);
	mask = ((mask & laneBits) << laneShiftsToMSB) >> Int4(31);
	return mask;
}

void PixelProgram::setBuiltins(Int &x, Int &y, Float4 (&z)[4], Float4 &w, Int cMask[4], const SampleSet &samples)
{
	routine.setImmutableInputBuiltins(spirvShader);

	// TODO(b/146486064): Consider only assigning these to the SpirvRoutine iff
	// they are ever going to be read.
	float x0 = 0.5f;
	float y0 = 0.5f;
	float x1 = 1.5f;
	float y1 = 1.5f;

	// "When Sample Shading is enabled, the x and y components of FragCoord reflect the
	//  location of one of the samples corresponding to the shader invocation. Otherwise,
	//  the x and y components of FragCoord reflect the location of the center of the fragment."
	if(state.sampleShadingEnabled && state.multiSampleCount > 1)
	{
		x0 = Constants::VkSampleLocations4[samples[0]][0];
		y0 = Constants::VkSampleLocations4[samples[0]][1];
		x1 = 1.0f + x0;
		y1 = 1.0f + y0;
	}

	routine.fragCoord[0] = SIMD::Float(Float(x)) + SIMD::Float(x0, x1, x0, x1);
	routine.fragCoord[1] = SIMD::Float(Float(y)) + SIMD::Float(y0, y0, y1, y1);
	routine.fragCoord[2] = z[0];  // sample 0
	routine.fragCoord[3] = w;

	routine.invocationsPerSubgroup = SIMD::Width;
	routine.helperInvocation = ~maskAny(cMask, samples);
	routine.windowSpacePosition[0] = x + SIMD::Int(0, 1, 0, 1);
	routine.windowSpacePosition[1] = y + SIMD::Int(0, 0, 1, 1);
	routine.viewID = *Pointer<Int>(data + OFFSET(DrawData, viewID));

	// PointCoord formula reference: https://www.khronos.org/registry/vulkan/specs/1.2/html/vkspec.html#primsrast-points-basic
	// Note we don't add a 0.5 offset to x and y here (like for fragCoord) because pointCoordX/Y have 0.5 subtracted as part of the viewport transform.
	SIMD::Float pointSizeInv = SIMD::Float(*Pointer<Float>(primitive + OFFSET(Primitive, pointSizeInv)));
	routine.pointCoord[0] = SIMD::Float(0.5f) + pointSizeInv * (((SIMD::Float(Float(x)) + SIMD::Float(0.0f, 1.0f, 0.0f, 1.0f)) - SIMD::Float(*Pointer<Float>(primitive + OFFSET(Primitive, pointCoordX)))));
	routine.pointCoord[1] = SIMD::Float(0.5f) + pointSizeInv * (((SIMD::Float(Float(y)) + SIMD::Float(0.0f, 0.0f, 1.0f, 1.0f)) - SIMD::Float(*Pointer<Float>(primitive + OFFSET(Primitive, pointCoordY)))));

	routine.setInputBuiltin(spirvShader, spv::BuiltInViewIndex, [&](const SpirvShader::BuiltinMapping &builtin, Array<SIMD::Float> &value) {
		assert(builtin.SizeInComponents == 1);
		value[builtin.FirstComponent] = As<SIMD::Float>(SIMD::Int(routine.viewID));
	});

	routine.setInputBuiltin(spirvShader, spv::BuiltInFragCoord, [&](const SpirvShader::BuiltinMapping &builtin, Array<SIMD::Float> &value) {
		assert(builtin.SizeInComponents == 4);
		value[builtin.FirstComponent + 0] = routine.fragCoord[0];
		value[builtin.FirstComponent + 1] = routine.fragCoord[1];
		value[builtin.FirstComponent + 2] = routine.fragCoord[2];
		value[builtin.FirstComponent + 3] = routine.fragCoord[3];
	});

	routine.setInputBuiltin(spirvShader, spv::BuiltInPointCoord, [&](const SpirvShader::BuiltinMapping &builtin, Array<SIMD::Float> &value) {
		assert(builtin.SizeInComponents == 2);
		value[builtin.FirstComponent + 0] = routine.pointCoord[0];
		value[builtin.FirstComponent + 1] = routine.pointCoord[1];
	});

	routine.setInputBuiltin(spirvShader, spv::BuiltInSubgroupSize, [&](const SpirvShader::BuiltinMapping &builtin, Array<SIMD::Float> &value) {
		assert(builtin.SizeInComponents == 1);
		value[builtin.FirstComponent] = As<SIMD::Float>(SIMD::Int(SIMD::Width));
	});

	routine.setInputBuiltin(spirvShader, spv::BuiltInHelperInvocation, [&](const SpirvShader::BuiltinMapping &builtin, Array<SIMD::Float> &value) {
		assert(builtin.SizeInComponents == 1);
		value[builtin.FirstComponent] = As<SIMD::Float>(routine.helperInvocation);
	});
}

void PixelProgram::executeShader(Int cMask[4], Int sMask[4], Int zMask[4], const SampleSet &samples)
{
	routine.descriptorSets = data + OFFSET(DrawData, descriptorSets);
	routine.descriptorDynamicOffsets = data + OFFSET(DrawData, descriptorDynamicOffsets);
	routine.pushConstants = data + OFFSET(DrawData, pushConstants);
	routine.constants = *Pointer<Pointer<Byte>>(data + OFFSET(DrawData, constants));

	auto it = spirvShader->inputBuiltins.find(spv::BuiltInFrontFacing);
	if(it != spirvShader->inputBuiltins.end())
	{
		ASSERT(it->second.SizeInComponents == 1);
		auto frontFacing = Int4(*Pointer<Int>(primitive + OFFSET(Primitive, clockwiseMask)));
		routine.getVariable(it->second.Id)[it->second.FirstComponent] = As<Float4>(frontFacing);
	}

	it = spirvShader->inputBuiltins.find(spv::BuiltInSampleMask);
	if(it != spirvShader->inputBuiltins.end())
	{
		static_assert(SIMD::Width == 4, "Expects SIMD width to be 4");
		Int4 laneBits = Int4(1, 2, 4, 8);

		Int4 inputSampleMask = 0;
		for(unsigned int q : samples)
		{
			inputSampleMask |= Int4(1 << q) & CmpNEQ(Int4(cMask[q]) & laneBits, Int4(0));
		}

		routine.getVariable(it->second.Id)[it->second.FirstComponent] = As<Float4>(inputSampleMask);
		// Sample mask input is an array, as the spec contemplates MSAA levels higher than 32.
		// Fill any non-zero indices with 0.
		for(auto i = 1u; i < it->second.SizeInComponents; i++)
		{
			routine.getVariable(it->second.Id)[it->second.FirstComponent + i] = Float4(0);
		}
	}

	it = spirvShader->inputBuiltins.find(spv::BuiltInSampleId);
	if(it != spirvShader->inputBuiltins.end())
	{
		ASSERT(samples.size() == 1);
		int sampleId = samples[0];
		routine.getVariable(it->second.Id)[it->second.FirstComponent] =
		    As<SIMD::Float>(SIMD::Int(sampleId));
	}

	it = spirvShader->inputBuiltins.find(spv::BuiltInSamplePosition);
	if(it != spirvShader->inputBuiltins.end())
	{
		ASSERT(samples.size() == 1);
		int sampleId = samples[0];
		routine.getVariable(it->second.Id)[it->second.FirstComponent + 0] =
		    SIMD::Float((state.multiSampleCount > 1) ? Constants::VkSampleLocations4[sampleId][0] : 0.5f);
		routine.getVariable(it->second.Id)[it->second.FirstComponent + 1] =
		    SIMD::Float((state.multiSampleCount > 1) ? Constants::VkSampleLocations4[sampleId][1] : 0.5f);
	}

	// Note: all lanes initially active to facilitate derivatives etc. Actual coverage is
	// handled separately, through the cMask.
	auto activeLaneMask = SIMD::Int(0xFFFFFFFF);
	auto storesAndAtomicsMask = maskAny(cMask, sMask, zMask, samples);
	routine.killMask = 0;

	spirvShader->emit(&routine, activeLaneMask, storesAndAtomicsMask, descriptorSets, state.multiSampleCount);
	spirvShader->emitEpilog(&routine);
	// At the last invocation of the fragment shader, clear phi data.
	// TODO(b/178662288): Automatically clear phis through SpirvRoutine lifetime reduction.
	if(samples[0] == static_cast<int>(state.multiSampleCount - 1))
	{
		spirvShader->clearPhis(&routine);
	}

	for(int i = 0; i < RENDERTARGETS; i++)
	{
		c[i].x = routine.outputs[i * 4];
		c[i].y = routine.outputs[i * 4 + 1];
		c[i].z = routine.outputs[i * 4 + 2];
		c[i].w = routine.outputs[i * 4 + 3];
		outputMasks[i] = ((spirvShader->outputs[i * 4 + 0].Type != SpirvShader::ATTRIBTYPE_UNUSED) ? 0x1 : 0x0) |
		                 ((spirvShader->outputs[i * 4 + 1].Type != SpirvShader::ATTRIBTYPE_UNUSED) ? 0x2 : 0x0) |
		                 ((spirvShader->outputs[i * 4 + 2].Type != SpirvShader::ATTRIBTYPE_UNUSED) ? 0x4 : 0x0) |
		                 ((spirvShader->outputs[i * 4 + 3].Type != SpirvShader::ATTRIBTYPE_UNUSED) ? 0x8 : 0x0);
	}

	clampColor(c);

	if(spirvShader->getAnalysis().ContainsKill)
	{
		for(unsigned int q : samples)
		{
			cMask[q] &= ~routine.killMask;
		}
	}

	it = spirvShader->outputBuiltins.find(spv::BuiltInSampleMask);
	if(it != spirvShader->outputBuiltins.end())
	{
		auto outputSampleMask = As<SIMD::Int>(routine.getVariable(it->second.Id)[it->second.FirstComponent]);

		for(unsigned int q : samples)
		{
			cMask[q] &= SignMask(CmpNEQ(outputSampleMask & SIMD::Int(1 << q), SIMD::Int(0)));
		}
	}

	it = spirvShader->outputBuiltins.find(spv::BuiltInFragDepth);
	if(it != spirvShader->outputBuiltins.end())
	{
		for(unsigned int q : samples)
		{
			z[q] = routine.getVariable(it->second.Id)[it->second.FirstComponent];

			if(state.depthClamp)
			{
				z[q] = Min(Max(z[q], Float4(state.minDepthClamp)), Float4(state.maxDepthClamp));
			}
		}
	}
}

Bool PixelProgram::alphaTest(Int cMask[4], const SampleSet &samples)
{
	if(!state.alphaToCoverage)
	{
		return true;
	}

	alphaToCoverage(cMask, c[0].w, samples);

	Int pass = 0;
	for(unsigned int q : samples)
	{
		pass = pass | cMask[q];
	}

	return pass != 0x0;
}

void PixelProgram::rasterOperation(Pointer<Byte> cBuffer[4], Int &x, Int sMask[4], Int zMask[4], Int cMask[4], const SampleSet &samples)
{
	for(int index = 0; index < RENDERTARGETS; index++)
	{
		if(!state.colorWriteActive(index))
		{
			continue;
		}

		auto format = state.targetFormat[index];
		switch(format)
		{
		case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
		case VK_FORMAT_R5G6B5_UNORM_PACK16:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_R16G16_UNORM:
		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
			for(unsigned int q : samples)
			{
				Pointer<Byte> buffer = cBuffer[index] + q * *Pointer<Int>(data + OFFSET(DrawData, colorSliceB[index]));
				Vector4s color;

				color.x = convertFixed16(c[index].x, false);
				color.y = convertFixed16(c[index].y, false);
				color.z = convertFixed16(c[index].z, false);
				color.w = convertFixed16(c[index].w, false);

				alphaBlend(index, buffer, color, x);
				writeColor(index, buffer, x, color, sMask[q], zMask[q], cMask[q]);
			}
			break;
		case VK_FORMAT_R16_SFLOAT:
		case VK_FORMAT_R16G16_SFLOAT:
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_R32G32_SFLOAT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		case VK_FORMAT_R32_SINT:
		case VK_FORMAT_R32G32_SINT:
		case VK_FORMAT_R32G32B32A32_SINT:
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32G32_UINT:
		case VK_FORMAT_R32G32B32A32_UINT:
		case VK_FORMAT_R16_SINT:
		case VK_FORMAT_R16G16_SINT:
		case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R16_UINT:
		case VK_FORMAT_R16G16_UINT:
		case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R8_SINT:
		case VK_FORMAT_R8G8_SINT:
		case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_R8_UINT:
		case VK_FORMAT_R8G8_UINT:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_A8B8G8R8_UINT_PACK32:
		case VK_FORMAT_A8B8G8R8_SINT_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
			for(unsigned int q : samples)
			{
				Pointer<Byte> buffer = cBuffer[index] + q * *Pointer<Int>(data + OFFSET(DrawData, colorSliceB[index]));
				Vector4f color = c[index];

				alphaBlend(index, buffer, color, x);
				writeColor(index, buffer, x, color, sMask[q], zMask[q], cMask[q]);
			}
			break;
		default:
			UNSUPPORTED("VkFormat: %d", int(format));
		}
	}
}

void PixelProgram::clampColor(Vector4f oC[RENDERTARGETS])
{
	for(int index = 0; index < RENDERTARGETS; index++)
	{
		if(!state.colorWriteActive(index) && !(index == 0 && state.alphaToCoverage))
		{
			continue;
		}

		switch(state.targetFormat[index])
		{
		case VK_FORMAT_UNDEFINED:
			break;
		case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
		case VK_FORMAT_R5G6B5_UNORM_PACK16:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_R16G16_UNORM:
		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
			oC[index].x = Max(oC[index].x, Float4(0.0f));
			oC[index].x = Min(oC[index].x, Float4(1.0f));
			oC[index].y = Max(oC[index].y, Float4(0.0f));
			oC[index].y = Min(oC[index].y, Float4(1.0f));
			oC[index].z = Max(oC[index].z, Float4(0.0f));
			oC[index].z = Min(oC[index].z, Float4(1.0f));
			oC[index].w = Max(oC[index].w, Float4(0.0f));
			oC[index].w = Min(oC[index].w, Float4(1.0f));
			break;
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_R32G32_SFLOAT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		case VK_FORMAT_R32_SINT:
		case VK_FORMAT_R32G32_SINT:
		case VK_FORMAT_R32G32B32A32_SINT:
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32G32_UINT:
		case VK_FORMAT_R32G32B32A32_UINT:
		case VK_FORMAT_R16_SFLOAT:
		case VK_FORMAT_R16G16_SFLOAT:
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		case VK_FORMAT_R16_SINT:
		case VK_FORMAT_R16G16_SINT:
		case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R16_UINT:
		case VK_FORMAT_R16G16_UINT:
		case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R8_SINT:
		case VK_FORMAT_R8G8_SINT:
		case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_R8_UINT:
		case VK_FORMAT_R8G8_UINT:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_A8B8G8R8_UINT_PACK32:
		case VK_FORMAT_A8B8G8R8_SINT_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
			break;
		default:
			UNSUPPORTED("VkFormat: %d", int(state.targetFormat[index]));
		}
	}
}

}  // namespace sw
