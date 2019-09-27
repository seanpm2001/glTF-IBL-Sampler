#pragma once

#include <vector>
#include <vulkan/vulkan.h>
#include <ktx.h>
#include "ResultType.h"

namespace IBLLib
{
	class KtxImage
	{
	public:
		enum class Version
		{
			KTX1 = 0,
			KTX2 = 1,
		};

		KtxImage(Version _version, uint32_t _width, uint32_t _height, VkFormat _vkFormat,uint32_t _levels, bool _isCubeMap);

		Result writeFace(const std::vector<unsigned char>& _inData, uint32_t _side, uint32_t _level);

		Result compress(unsigned int _qualityLevel = 128u);
		Result compress(ktxBasisParams _params);

		Result save(const char* _pathOut);

		~KtxImage();
	private:
		Version m_version = Version::KTX1;
		ktxTexture* m_pTexture = nullptr;
		ktxTextureCreateInfo m_createInfo{};
	};

} // !IBLLIb