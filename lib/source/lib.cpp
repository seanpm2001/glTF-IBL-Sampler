#include "lib.h"
#include "vkHelper.h"
#include "ShaderCompiler.h"
#include "STBImage.h"
#include "FileHelper.h"
#include "KtxImage.h"
#include <vk_format_utils.h>

namespace IBLLib
{
	Result uploadImage(vkHelper& _vulkan, const char* _inputPath, VkImage& _outImage)
	{
		_outImage = VK_NULL_HANDLE;
		STBImage panorama;

		if (panorama.loadHdr(_inputPath) != Result::Success)
		{
			return Result::InputPanoramaFileNotFound;
		}

		VkCommandBuffer uploadCmds = VK_NULL_HANDLE;
		if (_vulkan.createCommandBuffer(uploadCmds) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		// create staging buffer for image data
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		if (_vulkan.createBufferAndAllocate(stagingBuffer, static_cast<uint32_t>(panorama.getByteSize()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		// transfer data to the host coherent staging buffer 
		if (_vulkan.writeBufferData(stagingBuffer, panorama.getHdrData(), panorama.getByteSize()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		// create the destination image we want to sample in the shader
		if (_vulkan.createImage2DAndAllocate(_outImage, panorama.getWidth(), panorama.getHeight(), VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		if (_vulkan.beginCommandBuffer(uploadCmds, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		// transition to write dst layout
		_vulkan.transitionImageToTransferWrite(uploadCmds, _outImage);
		_vulkan.copyBufferToBasicImage2D(uploadCmds, stagingBuffer, _outImage);
		_vulkan.transitionImageToShaderRead(uploadCmds, _outImage);

		if (_vulkan.endCommandBuffer(uploadCmds) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		if (_vulkan.executeCommandBuffer(uploadCmds) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		_vulkan.destroyBuffer(stagingBuffer);
		_vulkan.destroyCommandBuffer(uploadCmds);

		return Result::Success;
	}

	Result convertVkFormat(vkHelper& _vulkan, VkCommandBuffer& _commandBuffer, const VkImage _srcImage, VkImage& _outImage, VkFormat _dstFormat, const VkImageLayout inputImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		const VkImageCreateInfo* pInfo = _vulkan.getCreateInfo(_srcImage);

		if (pInfo == nullptr)
		{
			return Result::InvalidArgument;
		}

		if (_outImage != VK_NULL_HANDLE)
		{
			printf("Expecting empty outImage\n");
			return Result::InvalidArgument;
		}

		const VkFormat srcFormat = pInfo->format; 
		const uint32_t sideLength = pInfo->extent.width;
		const uint32_t mipLevels = pInfo->mipLevels;
		const uint32_t arrayLayers = pInfo->arrayLayers;

		if (_vulkan.createImage2DAndAllocate(_outImage, sideLength, sideLength, _dstFormat,
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			mipLevels, arrayLayers, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		VkImageSubresourceRange subresourceRange{};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = mipLevels;
		subresourceRange.layerCount = arrayLayers;
	
		_vulkan.imageBarrier(_commandBuffer, _outImage,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,//src stage, access
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,//dst stage, access
			subresourceRange);

		_vulkan.imageBarrier(_commandBuffer, _srcImage,
			inputImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,//src stage, access
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,//dst stage, access
			subresourceRange);
	
		uint32_t currentSideLength = sideLength;

		for (uint32_t level = 0; level < mipLevels; level++)
		{
			VkImageBlit imageBlit{};

			// Source
			imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageBlit.srcSubresource.layerCount = arrayLayers;
			imageBlit.srcSubresource.mipLevel = level;
			imageBlit.srcOffsets[1].x = currentSideLength;
			imageBlit.srcOffsets[1].y = currentSideLength;
			imageBlit.srcOffsets[1].z = 1;

			// Destination
			imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageBlit.dstSubresource.layerCount = arrayLayers;
			imageBlit.dstSubresource.mipLevel = level;
			imageBlit.dstOffsets[1].x = currentSideLength;
			imageBlit.dstOffsets[1].y = currentSideLength;
			imageBlit.dstOffsets[1].z = 1;

			vkCmdBlitImage(
				_commandBuffer,
				_srcImage,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				_outImage,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&imageBlit,
				VK_FILTER_LINEAR);

			currentSideLength = currentSideLength >> 1;
		}

		return Result::Success;
	}

	Result downloadCubemap(vkHelper& _vulkan, const VkImage _srcImage, const char* _outputPath, KtxImage::Version _ktxVersion, unsigned int _ktxCompressionQuality, const VkImageLayout inputImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		const VkImageCreateInfo* pInfo = _vulkan.getCreateInfo(_srcImage);
		Result res = Success;
		if (pInfo == nullptr)
		{
			return Result::InvalidArgument;
		}

		const VkFormat cubeMapFormat = pInfo->format; 
		const uint32_t cubeMapFormatByteSize = FormatElementSize(cubeMapFormat);
		const uint32_t cubeMapSideLength = pInfo->extent.width;
		const uint32_t mipLevels = pInfo->mipLevels;

		using Faces = std::vector<VkBuffer>;
		using MipLevels = std::vector<Faces>;

		MipLevels stagingBuffer(mipLevels);

		{
			uint32_t currentSideLength = cubeMapSideLength;

			for (uint32_t level = 0; level < mipLevels; level++)
			{
				Faces& faces = stagingBuffer[level];
				faces.resize(6u);

				for (uint32_t face = 0; face < 6u; face++)
				{
					if (_vulkan.createBufferAndAllocate(
						faces[face], currentSideLength * currentSideLength * cubeMapFormatByteSize,
						VK_BUFFER_USAGE_TRANSFER_DST_BIT,// VkBufferUsageFlags _usage,
						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)//VkMemoryPropertyFlags _memoryFlags, 
						!= VK_SUCCESS)
					{
						return Result::VulkanError;
					}
				}

				currentSideLength = currentSideLength >> 1;
			}
		}

		VkCommandBuffer downloadCmds = VK_NULL_HANDLE;

		if (_vulkan.createCommandBuffer(downloadCmds) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		if (_vulkan.beginCommandBuffer(downloadCmds, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		// barrier on complete image
		VkImageSubresourceRange  subresourceRange{};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseArrayLayer = 0u;
		subresourceRange.layerCount = 6u;
		subresourceRange.baseMipLevel = 0u;
		subresourceRange.levelCount = mipLevels;

		_vulkan.imageBarrier(downloadCmds, _srcImage,
			inputImageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // src stage, access
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			subresourceRange);//dst stage, access

		// copy all faces & levels into staging buffers
		{
			uint32_t currentSideLength = cubeMapSideLength;

			VkBufferImageCopy region{};

			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1u;

			for (uint32_t level = 0; level < mipLevels; level++)
			{
				region.imageSubresource.mipLevel = level;
				Faces& faces = stagingBuffer[level];

				for (uint32_t face = 0; face < 6u; face++)
				{
					region.imageSubresource.baseArrayLayer = face;
					region.imageExtent = { currentSideLength , currentSideLength , 1u };

					_vulkan.copyImage2DToBuffer(downloadCmds, _srcImage, faces[face], region);
				}

				currentSideLength = currentSideLength >> 1;
			}
		}

		if (_vulkan.endCommandBuffer(downloadCmds) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		if (_vulkan.executeCommandBuffer(downloadCmds) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
		
		_vulkan.destroyCommandBuffer(downloadCmds);

		// Image is copied to buffer
		// Now map buffer and copy to ram
		{
			KtxImage ktxImage(_ktxVersion, cubeMapSideLength, cubeMapSideLength, cubeMapFormat, mipLevels, true);

			std::vector<unsigned char> imageData;

			uint32_t currentSideLength = cubeMapSideLength;

			for (uint32_t level = 0; level < mipLevels; level++)
			{
				const size_t imageByteSize = (size_t)currentSideLength * (size_t)currentSideLength * (size_t)cubeMapFormatByteSize;
				imageData.resize(imageByteSize);

				Faces& faces = stagingBuffer[level];

				for (uint32_t face = 0; face < 6u; face++)
				{
					if (_vulkan.readBufferData(faces[face], imageData.data(), imageByteSize) != VK_SUCCESS)
					{
						return Result::VulkanError;
					}

					res = ktxImage.writeFace(imageData, face, level);
					if (res != Result::Success)
					{
						return res;
					}

					_vulkan.destroyBuffer(faces[face]);
				}

				currentSideLength = currentSideLength >> 1;
			}

			if (_ktxCompressionQuality > 0)
			{
				res = ktxImage.compress(_ktxCompressionQuality);
				if (res != Result::Success)
				{
					printf("Compression failed\n");
					return res;
				}
			}

			res = ktxImage.save(_outputPath);
			if (res != Result::Success)
			{
				return res;
			}
		}

		return Result::Success;
	}

	void generateMipmapLevels(vkHelper& _vulkan, VkCommandBuffer& _commandBuffer, VkImage& _image, uint32_t _maxMipLevels, uint32_t _sideLength )
	{
		{ 
		VkImageSubresourceRange mipbaseRange = {};
		mipbaseRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		mipbaseRange.baseMipLevel = 0;
		mipbaseRange.levelCount = 1;
		mipbaseRange.layerCount = 6u;

		_vulkan.imageBarrier(_commandBuffer, _image,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,//dst stage, access
			mipbaseRange);
		}
		for (uint32_t i = 1; i < _maxMipLevels; i++)
		{
			VkImageBlit imageBlit{};

			// Source
			imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageBlit.srcSubresource.layerCount = 6u;
			imageBlit.srcSubresource.mipLevel = i - 1;
			imageBlit.srcOffsets[1].x = int32_t(_sideLength >> (i - 1));
			imageBlit.srcOffsets[1].y = int32_t(_sideLength >> (i - 1));
			imageBlit.srcOffsets[1].z = 1;
			
			// Destination
			imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageBlit.dstSubresource.layerCount = 6u;
			imageBlit.dstSubresource.mipLevel = i;
			imageBlit.dstOffsets[1].x = int32_t(_sideLength >> i);
			imageBlit.dstOffsets[1].y = int32_t(_sideLength >> i);
			imageBlit.dstOffsets[1].z = 1;

			VkImageSubresourceRange mipSubRange = {};
			mipSubRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			mipSubRange.baseMipLevel = i;
			mipSubRange.levelCount = 1;
			mipSubRange.layerCount = 6u;

			//  Transiton current mip level to transfer dest
			
			_vulkan.imageBarrier(_commandBuffer, _image,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,  
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				mipSubRange);//dst stage, access

			vkCmdBlitImage(
				_commandBuffer,
				_image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				_image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&imageBlit,
				VK_FILTER_LINEAR);


			//  Transiton  back
			_vulkan.imageBarrier(_commandBuffer, _image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,//dst stage, access
				mipSubRange);

		}
		
		{
			VkImageSubresourceRange completeRange = {};
			completeRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			completeRange.baseMipLevel = 0;
			completeRange.levelCount = _maxMipLevels;
			completeRange.layerCount = 6u;

			_vulkan.imageBarrier(_commandBuffer, _image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,//dst stage, access
				completeRange);
		} 
	}

	Result compileShader(vkHelper& _vulkan, const char* _path, const char* _entryPoint, VkShaderModule& _outModule, ShaderCompiler::Stage _stage)
	{
		std::vector<char>  glslBuffer;
		std::vector<uint32_t> outSpvBlob;

		if (readFile(_path, glslBuffer) == false)
		{
			return Result::ShaderFileNotFound;
		}

		if (ShaderCompiler::instance().compile(glslBuffer, _entryPoint, _stage, outSpvBlob) == false)
		{
			return Result::ShaderCompilationFailed;
		}

		if (_vulkan.loadShaderModule(_outModule, outSpvBlob.data(), outSpvBlob.size() * 4) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		return Result::Success;
	}
} // !IBLLib

IBLLib::Result IBLLib::sample(const char* _inputPath, const char* _outputPathSpecular, const char* _outputPathDiffuse, unsigned int _ktxVersion, unsigned int _ktxCompressionQuality, unsigned int _cubemapResolution, unsigned int _mipmapCount, unsigned int _sampleCount, OutputFormat _targetFormat)
{
	const VkFormat cubeMapFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	const uint32_t cubeMapSideLength = _cubemapResolution;
	const uint32_t outputMipLevels = _mipmapCount;

	uint32_t maxMipLevels = 0u;
	for (uint32_t m = cubeMapSideLength; m > 0; m = m >> 1, ++maxMipLevels) {}

	if (_cubemapResolution >> _mipmapCount < 1)
	{
		printf("Error: CubemapResolution incompatible with MipmapCount ");
		return Result::InvalidArgument;
	}

	IBLLib::Result res = Result::Success;

	vkHelper vulkan;

	if (vulkan.initialize() != VK_SUCCESS)
	{
		return Result::VulkanInitializationFailed;
	}

	VkImage panoramaImage;
	if ((res = uploadImage(vulkan, _inputPath, panoramaImage)) != Result::Success)
	{
		return res;
	}

	VkShaderModule fullscreenVertexShader = VK_NULL_HANDLE;
	if ((res = compileShader(vulkan, IBLSAMPLER_SHADERS_DIR "/primitive.vert", "main", fullscreenVertexShader, ShaderCompiler::Stage::Vertex)) != Result::Success)
	{
		return res;
	}

	VkShaderModule panoramaToCubeMapFragmentShader = VK_NULL_HANDLE;
	if ((res = compileShader(vulkan, IBLSAMPLER_SHADERS_DIR  "/filter.frag", "panoramaToCubeMap", panoramaToCubeMapFragmentShader, ShaderCompiler::Stage::Fragment)) != Result::Success)
	{
		return res;
	}

	VkShaderModule filterCubeMapSpecular = VK_NULL_HANDLE;
	if ((res = compileShader(vulkan, IBLSAMPLER_SHADERS_DIR "/filter.frag", "filterCubeMapSpecular", filterCubeMapSpecular, ShaderCompiler::Stage::Fragment)) != Result::Success)
	{
		return res;
	}

	VkShaderModule filterCubeMapDiffuse = VK_NULL_HANDLE;
	if ((res = compileShader(vulkan, IBLSAMPLER_SHADERS_DIR "/filter.frag", "filterCubeMapDiffuse", filterCubeMapDiffuse, ShaderCompiler::Stage::Fragment)) != Result::Success)
	{
		return res;
	}

	VkSampler panoramaSampler = VK_NULL_HANDLE;
	VkSampler cubeMipMapSampler = VK_NULL_HANDLE;

	{
		VkSamplerCreateInfo samplerInfo{};

		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; // ignore W

		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.mipLodBias = 0.f;
		samplerInfo.minLod = 0.f;
		samplerInfo.maxLod =  float(maxMipLevels+1);

		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 0.f;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

		if (vulkan.createSampler(panoramaSampler, samplerInfo) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		if (vulkan.createSampler(cubeMipMapSampler, samplerInfo) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkImageView panoramaImageView = VK_NULL_HANDLE;
	if (vulkan.createImageView(panoramaImageView, panoramaImage) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	//VK_IMAGE_USAGE_TRANSFER_SRC_BIT needed for transfer to staging buffer
	VkImage inputCubeMap = VK_NULL_HANDLE;
	if (vulkan.createImage2DAndAllocate(inputCubeMap, cubeMapSideLength, cubeMapSideLength, cubeMapFormat,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		maxMipLevels, 6u, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	VkImageView inputCubeMapCompleteView = VK_NULL_HANDLE;
	if (vulkan.createImageView(inputCubeMapCompleteView, inputCubeMap, { VK_IMAGE_ASPECT_COLOR_BIT, 0u, maxMipLevels, 0u, 6u }, VK_FORMAT_UNDEFINED, VK_IMAGE_VIEW_TYPE_CUBE) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	
	VkImage outputSpecularCubeMap = VK_NULL_HANDLE;
	if (vulkan.createImage2DAndAllocate(outputSpecularCubeMap, cubeMapSideLength, cubeMapSideLength, cubeMapFormat,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		outputMipLevels, 6u, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	std::vector< std::vector<VkImageView> > outputSpecularCubeMapViews(outputMipLevels);

	for (uint32_t i = 0; i < outputMipLevels; ++i)
	{
		outputSpecularCubeMapViews[i].resize(6, VK_NULL_HANDLE); //sides of the cube

		for (uint32_t j = 0; j < 6; j++)
		{
			VkImageSubresourceRange subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
			subresourceRange.baseMipLevel = i;
			subresourceRange.baseArrayLayer = j;
			if (vulkan.createImageView(outputSpecularCubeMapViews[i][j], outputSpecularCubeMap, subresourceRange) != VK_SUCCESS)
			{
				return Result::VulkanError;
			}
		}
	}

	VkImageView outputSpecularCubeMapCompleteView = VK_NULL_HANDLE;
	{
		VkImageSubresourceRange subresourceRange;
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseArrayLayer = 0u;
		subresourceRange.baseMipLevel = 0u;
		subresourceRange.layerCount = 6u;
		subresourceRange.levelCount = outputMipLevels;

		if (vulkan.createImageView(outputSpecularCubeMapCompleteView, outputSpecularCubeMap, subresourceRange, VK_FORMAT_UNDEFINED, VK_IMAGE_VIEW_TYPE_CUBE) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkImage outputDiffuseCubeMap = VK_NULL_HANDLE;
	if (vulkan.createImage2DAndAllocate(outputDiffuseCubeMap, cubeMapSideLength, cubeMapSideLength, cubeMapFormat,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		1u, 6u, VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	std::vector<VkImageView> outputDiffuseCubeMapViews(6u, VK_NULL_HANDLE);
	for (size_t i = 0; i < outputDiffuseCubeMapViews.size(); i++)
	{
		if (vulkan.createImageView(outputDiffuseCubeMapViews[i], outputDiffuseCubeMap, { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, static_cast<uint32_t>(i), 1u }) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkRenderPass renderPass = VK_NULL_HANDLE;
	{
		RenderPassDesc renderPassDesc;

		// add rendertargets (cubemap faces)
		for (int face = 0; face < 6; ++face)
		{
			renderPassDesc.addAttachment(cubeMapFormat); // RGB only? (change stb loading as well)
		}

		if (vulkan.createRenderPass(renderPass, renderPassDesc.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

#pragma region PanoramaToCubeMap
	////////////////////////////////////////////////////////////////////////////////////////
	 // Panorama to CubeMap
	VkPipelineLayout panoramaPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSet panoramaSet = VK_NULL_HANDLE;
	VkPipeline panoramaToCubeMapPipeline = VK_NULL_HANDLE;
	{
		//
		// Create pipeline layout
		//
		VkDescriptorSetLayout panoramaSetLayout = VK_NULL_HANDLE;
		DescriptorSetInfo setLayout0;
		setLayout0.addCombinedImageSampler(panoramaSampler, panoramaImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		if (setLayout0.create(vulkan, panoramaSetLayout, panoramaSet) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		vulkan.updateDescriptorSets(setLayout0.getWrites());

		if (vulkan.createPipelineLayout(panoramaPipelineLayout, panoramaSetLayout) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		GraphicsPipelineDesc panormaToCubePipeline;

		panormaToCubePipeline.addShaderStage(fullscreenVertexShader, VK_SHADER_STAGE_VERTEX_BIT, "main");
		panormaToCubePipeline.addShaderStage(panoramaToCubeMapFragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT, "panoramaToCubeMap");

		panormaToCubePipeline.setRenderPass(renderPass);
		panormaToCubePipeline.setPipelineLayout(panoramaPipelineLayout);

		for (int face = 0; face < 6; ++face)
		{
			VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
			colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			colorBlendAttachment.blendEnable = VK_FALSE;
			colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
			colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
			colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
			colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
			colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
			colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional

			panormaToCubePipeline.addColorBlendAttachment(colorBlendAttachment); // RGB only? (change stb loading as well)
		}

		panormaToCubePipeline.setViewportExtent(VkExtent2D{ cubeMapSideLength, cubeMapSideLength });

		if (vulkan.createPipeline(panoramaToCubeMapPipeline, panormaToCubePipeline.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}
#pragma endregion

	//Push Constants for specular and diffuse filter passes  
	struct PushConstant
	{
		float roughness = 0.f;
		uint32_t sampleCount = 1u;
		uint32_t mipLevel = 1u;
		uint32_t width = 1024u;
	};

	std::vector<VkPushConstantRange> ranges(1u);
	VkPushConstantRange& range = ranges.front();

	range.offset = 0u;
	range.size = sizeof(PushConstant);
	range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;


	////////////////////////////////////////////////////////////////////////////////////////
	// Specular Filter CubeMap
	VkDescriptorSet specularDescriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout specularFilterPipelineLayout = VK_NULL_HANDLE;
	VkPipeline specularFilterPipeline = VK_NULL_HANDLE;

	{
		DescriptorSetInfo setLayout1;
		uint32_t binding = 1u;
		setLayout1.addCombinedImageSampler(panoramaSampler, inputCubeMapCompleteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, binding, VK_SHADER_STAGE_FRAGMENT_BIT); // change sampler ?
		//Binding ist set to 1!!

		VkDescriptorSetLayout specularSetLayout = VK_NULL_HANDLE;
		if (setLayout1.create(vulkan, specularSetLayout, specularDescriptorSet) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		vulkan.updateDescriptorSets(setLayout1.getWrites());

		if (vulkan.createPipelineLayout(specularFilterPipelineLayout, specularSetLayout, ranges) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		GraphicsPipelineDesc filterCubeMapPipelineDesc;

		filterCubeMapPipelineDesc.addShaderStage(fullscreenVertexShader, VK_SHADER_STAGE_VERTEX_BIT, "main");
		filterCubeMapPipelineDesc.addShaderStage(filterCubeMapSpecular, VK_SHADER_STAGE_FRAGMENT_BIT, "filterCubeMapSpecular");

		filterCubeMapPipelineDesc.setRenderPass(renderPass);
		filterCubeMapPipelineDesc.setPipelineLayout(specularFilterPipelineLayout);

		for (int face = 0; face < 6; ++face)
		{
			VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
			colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT; // TODO: rgb only
			colorBlendAttachment.blendEnable = VK_FALSE;
			colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
			colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
			colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
			colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
			colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
			colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional

			filterCubeMapPipelineDesc.addColorBlendAttachment(colorBlendAttachment); // RGB only? (change stb loading as well)
		}

		filterCubeMapPipelineDesc.setViewportExtent(VkExtent2D{ cubeMapSideLength, cubeMapSideLength });

		if (vulkan.createPipeline(specularFilterPipeline, filterCubeMapPipelineDesc.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}


	////////////////////////////////////////////////////////////////////////////////////////
	// Diffuse Filter 
	VkPipeline diffuseFilterPipeline = VK_NULL_HANDLE;
	VkPipelineLayout diffuseFilterPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSet diffuseDescriptorSet = VK_NULL_HANDLE;
	{
		//
		// Create pipeline layout
		//
		VkDescriptorSetLayout diffuseSetLayout = VK_NULL_HANDLE;
		DescriptorSetInfo setLayout0;
		uint32_t binding = 1u;
		setLayout0.addCombinedImageSampler(panoramaSampler, inputCubeMapCompleteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, binding);

		if (setLayout0.create(vulkan, diffuseSetLayout, diffuseDescriptorSet) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		vulkan.updateDescriptorSets(setLayout0.getWrites());

		if (vulkan.createPipelineLayout(diffuseFilterPipelineLayout, diffuseSetLayout, ranges) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		GraphicsPipelineDesc diffuseFilterPipelineDesc;

		diffuseFilterPipelineDesc.addShaderStage(fullscreenVertexShader, VK_SHADER_STAGE_VERTEX_BIT, "main");
		diffuseFilterPipelineDesc.addShaderStage(filterCubeMapDiffuse, VK_SHADER_STAGE_FRAGMENT_BIT, "filterCubeMapDiffuse");

		diffuseFilterPipelineDesc.setRenderPass(renderPass);
		diffuseFilterPipelineDesc.setPipelineLayout(diffuseFilterPipelineLayout);

		for (int face = 0; face < 6; ++face)
		{
			VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
			colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			colorBlendAttachment.blendEnable = VK_FALSE;
			colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
			colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
			colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
			colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
			colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
			colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional

			diffuseFilterPipelineDesc.addColorBlendAttachment(colorBlendAttachment); // RGB only? (change stb loading as well)
		}

		diffuseFilterPipelineDesc.setViewportExtent(VkExtent2D{ cubeMapSideLength, cubeMapSideLength });

		if (vulkan.createPipeline(diffuseFilterPipeline, diffuseFilterPipelineDesc.getInfo()) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}
	////////////////////////////////////////////////////////////////////////////////////////
	// Transform panorama image to cube map
	VkCommandBuffer cubeMapCmd;

	if (vulkan.createCommandBuffer(cubeMapCmd) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (vulkan.beginCommandBuffer(cubeMapCmd, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	std::vector<VkImageView> inputCubeMapViews(6u, VK_NULL_HANDLE);
	for (size_t i = 0; i < inputCubeMapViews.size(); i++)
	{
		if (vulkan.createImageView(inputCubeMapViews[i], inputCubeMap, { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, static_cast<uint32_t>(i), 1u }) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}
	}

	VkFramebuffer cubeMapInputFramebuffer = VK_NULL_HANDLE;
	if (vulkan.createFramebuffer(cubeMapInputFramebuffer, renderPass, cubeMapSideLength, cubeMapSideLength, inputCubeMapViews, 1u) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	{
	VkImageSubresourceRange  subresourceRangeBaseMiplevel = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, maxMipLevels, 0u, 6u };

	vulkan.imageBarrier(cubeMapCmd, inputCubeMap,
		VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,// src stage, access	
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, //dst stage, access
		subresourceRangeBaseMiplevel);
	}

	vulkan.bindDescriptorSet(cubeMapCmd, panoramaPipelineLayout, panoramaSet);

	vkCmdBindPipeline(cubeMapCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, panoramaToCubeMapPipeline);

	std::vector<VkClearValue> clearValues;
	for (size_t i = 0; i < inputCubeMapViews.size(); i++)
	{
		clearValues.push_back({ 0.0f, 0.0f, 1.0f, 1.0f });
	}
	vulkan.beginRenderPass(cubeMapCmd, renderPass, cubeMapInputFramebuffer, VkRect2D{ 0u, 0u, cubeMapSideLength, cubeMapSideLength }, clearValues);
	vkCmdDraw(cubeMapCmd, 3, 1u, 0, 0);
	vulkan.endRenderPass(cubeMapCmd);

	////////////////////////////////////////////////////////////////////////////////////////
	//Generate MipLevels
	generateMipmapLevels(vulkan, cubeMapCmd, inputCubeMap, maxMipLevels, cubeMapSideLength);

	// Filter specular
	vulkan.bindDescriptorSet(cubeMapCmd, specularFilterPipelineLayout, specularDescriptorSet);

	vkCmdBindPipeline(cubeMapCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, specularFilterPipeline);

	unsigned int currentFramebufferSideLength = cubeMapSideLength;
	//Filter every mip level: from currentMipLevel->currentMipLevel+1
	for (uint32_t currentMipLevel = 0; currentMipLevel < outputMipLevels; currentMipLevel++)
	{
		//Framebuffer will be destroyed automatically at shutdown
		VkFramebuffer cubeMapOutputFramebuffer = VK_NULL_HANDLE;
		if (vulkan.createFramebuffer(cubeMapOutputFramebuffer, renderPass, currentFramebufferSideLength, currentFramebufferSideLength, outputSpecularCubeMapViews[currentMipLevel], 1u) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		VkImageSubresourceRange  subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, currentMipLevel, 1u, 0u, 6u };

		vulkan.imageBarrier(cubeMapCmd, outputSpecularCubeMap,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,//src stage, access
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // dst stage, access		
			subresourceRange);

		PushConstant values;
		values.roughness = currentMipLevel / static_cast<float>(outputMipLevels);
		values.sampleCount = _sampleCount;
		values.mipLevel = currentMipLevel;
		values.width = currentFramebufferSideLength;

		vkCmdPushConstants(cubeMapCmd, specularFilterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstant), &values);

		vulkan.beginRenderPass(cubeMapCmd, renderPass, cubeMapOutputFramebuffer, VkRect2D{ 0u, 0u, currentFramebufferSideLength, currentFramebufferSideLength }, clearValues);
		vkCmdDraw(cubeMapCmd, 3, 1u, 0, 0);
		vulkan.endRenderPass(cubeMapCmd);
		 

		currentFramebufferSideLength = currentFramebufferSideLength >> 1;
	}

	// Filter diffuse
	{
		VkFramebuffer diffuseCubeMapFramebuffer = VK_NULL_HANDLE;
		if (vulkan.createFramebuffer(diffuseCubeMapFramebuffer, renderPass, cubeMapSideLength, cubeMapSideLength, outputDiffuseCubeMapViews, 1u) != VK_SUCCESS)
		{
			return Result::VulkanError;
		}

		VkImageSubresourceRange  subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1u, 0u, 6u };

		vulkan.imageBarrier(cubeMapCmd, outputDiffuseCubeMap,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,//src stage, access
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // dst stage, access		
			subresourceRange);


		vulkan.bindDescriptorSet(cubeMapCmd, diffuseFilterPipelineLayout, diffuseDescriptorSet);

		vkCmdBindPipeline(cubeMapCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, diffuseFilterPipeline);
		
		PushConstant values;
		values.roughness = 0;
		values.sampleCount = _sampleCount;
		values.mipLevel = 0;
		values.width = cubeMapSideLength;

		vkCmdPushConstants(cubeMapCmd, diffuseFilterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstant), &values);

		vulkan.beginRenderPass(cubeMapCmd, renderPass, diffuseCubeMapFramebuffer, VkRect2D{ 0u, 0u, cubeMapSideLength, cubeMapSideLength }, clearValues);
		vkCmdDraw(cubeMapCmd, 3, 1u, 0, 0);
		vulkan.endRenderPass(cubeMapCmd);
	}

	////////////////////////////////////////////////////////////////////////////////////////
	//Output

	VkFormat targetFormat = static_cast<VkFormat>(_targetFormat);
	VkImageLayout currentSpecularCubeMapImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkImageLayout currentDiffuseCubeMapImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkImage convertedSpecularCubeMap = VK_NULL_HANDLE;
	VkImage convertedDiffuseCubeMap = VK_NULL_HANDLE;

	if(targetFormat != cubeMapFormat)
	{		
		if ((res = convertVkFormat(vulkan, cubeMapCmd, outputSpecularCubeMap, convertedSpecularCubeMap, targetFormat, currentSpecularCubeMapImageLayout)) != Success)
		{
			printf("Failed to convert Image \n");
			return res;
		}
		currentSpecularCubeMapImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	
		if ((res = convertVkFormat(vulkan, cubeMapCmd, outputDiffuseCubeMap, convertedDiffuseCubeMap, targetFormat, currentDiffuseCubeMapImageLayout)) != Success)
		{
			printf("Failed to convert Image \n");
			return res;
		}
		currentDiffuseCubeMapImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
	else 
	{
		convertedSpecularCubeMap = outputSpecularCubeMap;
		convertedDiffuseCubeMap = outputDiffuseCubeMap;
	}

	if (vulkan.endCommandBuffer(cubeMapCmd) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}

	if (vulkan.executeCommandBuffer(cubeMapCmd) != VK_SUCCESS)
	{
		return Result::VulkanError;
	}	
	
	KtxImage::Version ktxImageVersion = KtxImage::Version::KTX1;

	switch (_ktxVersion)
	{
	case 1:
		ktxImageVersion = KtxImage::Version::KTX1;
		break;
	case 2:
		ktxImageVersion = KtxImage::Version::KTX2;
		break;
	}

	if (downloadCubemap(vulkan, convertedSpecularCubeMap, _outputPathSpecular, ktxImageVersion, _ktxCompressionQuality, currentSpecularCubeMapImageLayout) != VK_SUCCESS)
	{
		printf("Failed to download Image \n");
		return Result::VulkanError;
	}


	if (downloadCubemap(vulkan, convertedDiffuseCubeMap, _outputPathDiffuse, ktxImageVersion, _ktxCompressionQuality, currentDiffuseCubeMapImageLayout) != VK_SUCCESS)
	{
		printf("Failed to download Image \n");
		return Result::VulkanError;
	}


	return Result::Success;
}