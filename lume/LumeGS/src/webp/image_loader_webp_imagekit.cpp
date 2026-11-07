/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// Original author: Jarno Paananen.

#include "image_loader_webp.h"

#include <cstring>
#include <iterator>
#include <limits>
#include <memory>

#include <base/math/mathf.h>
#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/namespace.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <plugin_gs/util/gs_limits.h>

using namespace BASE_NS;
using namespace CORE_NS;

namespace WebPPlugin {
namespace {

constexpr uint32_t COMPONENT_COUNT = 4u;
constexpr uint32_t BYTES_PER_COMPONENT = 1u;

IImageLoaderManager::LoadResult ResultFailure(const string_view error)
{
    IImageLoaderManager::LoadResult result {
        false,  // success
        "",     // error[128];
        nullptr // image;
    };

    // Copy the error string
    const auto count = Math::min(error.size(), sizeof(result.error) - 1);
    error.copy(result.error, count);
    result.error[count] = '\0';

    return result;
}

IImageLoaderManager::LoadResult ResultSuccess(IImageContainer::Ptr image)
{
    return IImageLoaderManager::LoadResult {
        true,                // success
        "",                  // error[128];
        BASE_NS::move(image) // image;
    };
}

IImageLoaderManager::LoadAnimatedResult ResultFailureAnimated(const string_view error)
{
    IImageLoaderManager::LoadAnimatedResult result {
        false,  // success
        "",     // error[128];
        nullptr // image;
    };

    // Copy the error string
    const auto count = Math::min(error.size(), sizeof(result.error) - 1);
    error.copy(result.error, count);
    result.error[count] = '\0';

    return result;
}

} // namespace

class WebPImage final : public IImageContainer {
public:
    WebPImage() : IImageContainer(), imageDesc_(), imageBuffer_() {}

    ~WebPImage() override = default;

    using Ptr = BASE_NS::unique_ptr<WebPImage, Deleter>;

    const ImageDesc& GetImageDesc() const override
    {
        return imageDesc_;
    }

    array_view<const uint8_t> GetData() const override
    {
        return array_view<const uint8_t>(imageBytes_.get(), imageBytesLength_);
    }

    array_view<const SubImageDesc> GetBufferImageCopies() const override
    {
        return array_view<const SubImageDesc>(&imageBuffer_, 1);
    }

    static constexpr Format ResolveFormat(uint32_t loadFlags)
    {
        const bool forceLinear = (loadFlags & IImageLoaderManager::IMAGE_LOADER_FORCE_LINEAR_RGB_BIT) != 0;
        return forceLinear ? Format::BASE_FORMAT_R8G8B8A8_UNORM : Format::BASE_FORMAT_R8G8B8A8_SRGB;
    }

    constexpr static ImageDesc ResolveImageDesc(Format format, uint32_t imageWidth, uint32_t imageHeight,
        uint32_t bitsPerPixel, bool generateMips, bool isPremultiplied)
    {
        uint32_t mipCount = 1;

        // 1D images not supported with WebP loader
        constexpr ImageType imageType = ImageType::TYPE_2D;
        constexpr ImageViewType imageViewType = ImageViewType::VIEW_TYPE_2D;

        uint32_t imageFlags = isPremultiplied ? ImageFlags::FLAGS_PREMULTIPLIED_ALPHA_BIT : 0;

        if (generateMips) {
            imageFlags |= ImageFlags::FLAGS_REQUESTING_MIPMAPS_BIT;
            uint32_t mipsize = (imageWidth > imageHeight) ? imageWidth : imageHeight;
            mipCount = 0;
            while (mipsize > 0) {
                mipCount++;
                mipsize >>= 1;
            }
        }

        return ImageDesc {
            imageFlags,    // imageFlags
            1,             // blockPixelWidth
            1,             // blockPixelHeight
            1,             // blockPixelDepth
            bitsPerPixel,  // bitsPerBlock
            imageType,     // imageType
            imageViewType, // imageViewType
            format,        // format
            imageWidth,    // width
            imageHeight,   // height
            1,             // depth
            mipCount,      // mipCount
            1,             // layerCount
        };
    }

    // The platform decoder returns premultiplied alpha. SOG stores data in all
    // four channels (opacity, quaternion tag), so premultiplying corrupts RGB.
    // Undo it: p = floor(v * a / 255)  =>  v ~= (p * 255 + a/2) / a.
    static void Unpremultiply(uint8_t* pixels, uint32_t width, uint32_t height, size_t rowBytes)
    {
        for (uint32_t y = 0; y < height; y++) {
            uint8_t* row = pixels + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < width; x++) {
                uint8_t* px = row + static_cast<size_t>(x) * COMPONENT_COUNT;
                const uint32_t alpha = px[3];
                if (alpha == 0 || alpha == 255) {
                    continue;
                }
                for (uint32_t c = 0; c < 3u; c++) {
                    const uint32_t v = ((uint32_t)px[c] * 255u + alpha / 2u) / alpha;
                    px[c] = static_cast<uint8_t>(v > 255u ? 255u : v);
                }
            }
        }
    }

    static IImageLoaderManager::LoadResult Load(array_view<const uint8_t> imageFileBytes, uint32_t loadFlags)
    {
        if (imageFileBytes.empty()) {
            return ResultFailure("Input data must not be null.");
        }
        if (imageFileBytes.size() > GS::MAX_WEBP_FILE_SIZE) {
            return ResultFailure("Invalid file size");
        }

        OH_ImageSourceNative* source = nullptr;
        Image_ErrorCode result = OH_ImageSourceNative_CreateFromData(
            const_cast<uint8_t*>(imageFileBytes.data()), imageFileBytes.size(), &source);
        if (result != IMAGE_SUCCESS || source == nullptr) {
            return ResultFailure("Loading image failed");
        }

        OH_DecodingOptions* options = nullptr;
        OH_DecodingOptions_Create(&options);
        OH_DecodingOptions_SetPixelFormat(options, PIXEL_FORMAT_RGBA_8888);

        OH_PixelmapNative* pixelmap = nullptr;
        result = OH_ImageSourceNative_CreatePixelmap(source, options, &pixelmap);
        OH_DecodingOptions_Release(options);
        OH_ImageSourceNative_Release(source);
        if (result != IMAGE_SUCCESS || pixelmap == nullptr) {
            return ResultFailure("Loading image failed");
        }

        OH_Pixelmap_ImageInfo* info = nullptr;
        OH_PixelmapImageInfo_Create(&info);
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowStride = 0;
        int32_t pixelFormat = 0;
        OH_PixelmapNative_GetImageInfo(pixelmap, info);
        OH_PixelmapImageInfo_GetWidth(info, &width);
        OH_PixelmapImageInfo_GetHeight(info, &height);
        OH_PixelmapImageInfo_GetRowStride(info, &rowStride);
        OH_PixelmapImageInfo_GetPixelFormat(info, &pixelFormat);
        OH_PixelmapImageInfo_Release(info);

        if (width == 0 || height == 0 || width > GS::MAX_IMAGE_DIM || height > GS::MAX_IMAGE_DIM) {
            OH_PixelmapNative_Release(pixelmap);
            return ResultFailure("Image dimensions exceed allowed limit");
        }
        if (pixelFormat != PIXEL_FORMAT_RGBA_8888) {
            OH_PixelmapNative_Release(pixelmap);
            return ResultFailure("Unsupported decoded pixel format");
        }
        if (rowStride < width * COMPONENT_COUNT) {
            rowStride = width * COMPONENT_COUNT;
        }

        const bool premultiplyRequested =
            (loadFlags & IImageLoaderManager::IMAGE_LOADER_PREMULTIPLY_ALPHA) != 0u;
        const bool generateMips = (loadFlags & IImageLoaderManager::IMAGE_LOADER_GENERATE_MIPS) != 0u;
        const bool flipVertically = (loadFlags & IImageLoaderManager::IMAGE_LOADER_FLIP_VERTICALLY_BIT) != 0u;
        const Format format = ResolveFormat(loadFlags);
        constexpr uint32_t bitsPerPixel = BYTES_PER_COMPONENT * COMPONENT_COUNT * 8u;

        auto image = WebPImage::Ptr(new WebPImage);
        if (!image) {
            OH_PixelmapNative_Release(pixelmap);
            return ResultFailure("Loading image failed.");
        }
        image->imageDesc_ =
            ResolveImageDesc(format, width, height, bitsPerPixel, generateMips, premultiplyRequested);
        image->imageBuffer_ = SubImageDesc {
            0,        // uint32_t bufferOffset
            width,    // uint32_t bufferRowLength
            height,   // uint32_t bufferImageHeight
            0,        // uint32_t mipLevel
            1,        // uint32_t layerCount
            width,    // uint32_t width
            height,   // uint32_t height
            1,        // uint32_t depth
        };

        // Metadata only: dimensions and format are enough, no pixel readback.
        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_METADATA_ONLY) != 0u) {
            OH_PixelmapNative_Release(pixelmap);
            image->imageBytesLength_ = 0;
            return ResultSuccess(BASE_NS::move(image));
        }

        const size_t sourceSize = static_cast<size_t>(rowStride) * height;
        auto sourcePixels = BASE_NS::make_unique<uint8_t[]>(sourceSize);
        size_t bufferSize = sourceSize;
        result = OH_PixelmapNative_ReadPixels(pixelmap, sourcePixels.get(), &bufferSize);
        OH_PixelmapNative_Release(pixelmap);
        if (result != IMAGE_SUCCESS) {
            return ResultFailure("Reading image pixels failed");
        }

        // Tightly packed RGBA8888, optional vertical flip, optional
        // unpremultiply (ImageKit always decodes with premultiplied alpha).
        const size_t rowBytes = static_cast<size_t>(width) * COMPONENT_COUNT;
        auto pixels = BASE_NS::make_unique<uint8_t[]>(rowBytes * height);
        if (!pixels) {
            return ResultFailure("Out of memory");
        }
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* src = sourcePixels.get() + static_cast<size_t>(y) * rowStride;
            const uint32_t dstY = flipVertically ? (height - 1u - y) : y;
            std::memcpy(pixels.get() + static_cast<size_t>(dstY) * rowBytes, src, rowBytes);
        }
        if (!premultiplyRequested) {
            Unpremultiply(pixels.get(), width, height, rowBytes);
        }

        image->imageBytes_ = BASE_NS::move(pixels);
        image->imageBytesLength_ = rowBytes * height;
        return ResultSuccess(BASE_NS::move(image));
    }

protected:
    void Destroy() override
    {
        delete this;
    }

private:
    ImageDesc imageDesc_;
    SubImageDesc imageBuffer_;

    BASE_NS::unique_ptr<uint8_t[]> imageBytes_;
    size_t imageBytesLength_ = 0;
};

class ImageLoaderWebP final : public IImageLoaderManager::IImageLoader {
public:
    // Inherited via ImageManager::IImageLoader
    IImageLoaderManager::LoadResult Load(IFile& file, uint32_t loadFlags) const override
    {
        const uint64_t byteLength = file.GetLength();
        if (byteLength == 0 || byteLength > GS::MAX_WEBP_FILE_SIZE) {
            return ResultFailure("Invalid file size");
        }

        // Read the file to a buffer.
        auto buffer = BASE_NS::make_unique<uint8_t[]>(static_cast<size_t>(byteLength));
        const uint64_t read = file.Read(buffer.get(), byteLength);
        if (read != byteLength) {
            return ResultFailure("Reading file failed.");
        }

        return WebPImage::Load(
            array_view<const uint8_t>(buffer.get(), static_cast<size_t>(byteLength)), loadFlags);
    }

    IImageLoaderManager::LoadResult Load(array_view<const uint8_t> imageFileBytes, uint32_t loadFlags) const override
    {
        return WebPImage::Load(imageFileBytes, loadFlags);
    }

    bool CanLoad(array_view<const uint8_t> imageFileBytes) const override
    {
        // Check for WebP
        if (imageFileBytes.size() >= 12 && imageFileBytes[0] == 'R' && imageFileBytes[1] == 'I' &&
            imageFileBytes[2] == 'F' && imageFileBytes[3] == 'F' && imageFileBytes[8] == 'W' &&
            imageFileBytes[9] == 'E' && imageFileBytes[10] == 'B' && imageFileBytes[11] == 'P') {
            return true;
        }
        return false;
    }

    // Animated WebP is not needed for the SOG/LumeGS assets. ImageKit decoding
    // of the still image above is used for every texture in a .sog container.
    IImageLoaderManager::LoadAnimatedResult LoadAnimatedImage(IFile& /*file*/, uint32_t /*loadFlags*/) override
    {
        return ResultFailureAnimated("Animated WebP is not supported");
    }

    IImageLoaderManager::LoadAnimatedResult LoadAnimatedImage(
        array_view<const uint8_t> /*imageFileBytes*/, uint32_t /*loadFlags*/) override
    {
        return ResultFailureAnimated("Animated WebP is not supported");
    }

    vector<IImageLoaderManager::ImageType> GetSupportedTypes() const override
    {
        return vector<IImageLoaderManager::ImageType>(std::begin(IMAGE_TYPES), std::end(IMAGE_TYPES));
    }

protected:
    ~ImageLoaderWebP() = default;
    void Destroy() override
    {
        delete this;
    }
};

IImageLoaderManager::IImageLoader::Ptr CreateImageLoaderWebP(CORE_NS::PluginToken)
{
    return IImageLoaderManager::IImageLoader::Ptr { new ImageLoaderWebP() };
}

} // namespace WebPPlugin
