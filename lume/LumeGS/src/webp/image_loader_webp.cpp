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

#include <functional>
#include <limits>
#include <memory>

#include <base/math/mathf.h>
#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/namespace.h>
#include <plugin_gs/util/gs_limits.h>

#include "webp/decode.h"
#include "webp/demux.h"

using namespace BASE_NS;
using namespace CORE_NS;

namespace WebPPlugin {
namespace {

void FreeWebPImage(WebPDecoderConfig* config)
{
    WebPFreeDecBuffer(&config->output);
    delete config;
}

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

IImageLoaderManager::LoadAnimatedResult ResultSuccessAnimated(IAnimatedImage::Ptr image)
{
    return IImageLoaderManager::LoadAnimatedResult {
        true,                // success
        "",                  // error[128];
        BASE_NS::move(image) // image;
    };
}

using WebPImageDeleter = std::function<void(WebPDecoderConfig*)>;
using WebPImagePtr = unique_ptr<WebPDecoderConfig, WebPImageDeleter>;
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
        return array_view<const uint8_t>(
            static_cast<const uint8_t*>(imageBytes_.get()->output.u.RGBA.rgba), imageBytesLength_);
    }

    array_view<const SubImageDesc> GetBufferImageCopies() const override
    {
        return array_view<const SubImageDesc>(&imageBuffer_, 1);
    }

    static constexpr Format ResolveFormat(uint32_t loadFlags, uint32_t componentCount, bool is16bpc)
    {
        Format format {};
        const bool forceLinear = (loadFlags & IImageLoaderManager::IMAGE_LOADER_FORCE_LINEAR_RGB_BIT) != 0;

        switch (componentCount) {
            case 1u:
                format = is16bpc ? Format::BASE_FORMAT_R16_UNORM
                                 : (forceLinear ? Format::BASE_FORMAT_R8_UNORM : Format::BASE_FORMAT_R8_SRGB);
                break;
            case 2u:
                format = is16bpc ? Format::BASE_FORMAT_R16G16_UNORM
                                 : (forceLinear ? Format::BASE_FORMAT_R8G8_UNORM : Format::BASE_FORMAT_R8G8_SRGB);
                break;
            case 3u:
                format = is16bpc ? Format::BASE_FORMAT_R16G16B16_UNORM
                                 : (forceLinear ? Format::BASE_FORMAT_R8G8B8_UNORM : Format::BASE_FORMAT_R8G8B8_SRGB);
                break;
            case 4u:
                format = is16bpc
                             ? Format::BASE_FORMAT_R16G16B16A16_UNORM
                             : (forceLinear ? Format::BASE_FORMAT_R8G8B8A8_UNORM : Format::BASE_FORMAT_R8G8B8A8_SRGB);
                break;
            default:
                format = Format::BASE_FORMAT_UNDEFINED;
                break;
        }

        return format;
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

    static IImageLoaderManager::LoadResult CreateImage(
        WebPImagePtr imageBytes, uint32_t imageWidth, uint32_t imageHeight, uint32_t componentCount, uint32_t loadFlags)
    {
        auto image = WebPImage::Ptr(new WebPImage);
        if (!image) {
            return ResultFailure("Loading image failed.");
        }

        // Premultiply alpha if requested.
        bool isPremultiplied = false;
        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_PREMULTIPLY_ALPHA) != 0) {
            isPremultiplied = true;
        }

        const Format format = ResolveFormat(loadFlags, componentCount, false);

        const bool generateMips = (loadFlags & IImageLoaderManager::IMAGE_LOADER_GENERATE_MIPS) != 0;
        const uint32_t bytesPerComponent = 1u;
        const uint32_t bitsPerPixel = bytesPerComponent * componentCount * 8u;

        image->imageDesc_ =
            ResolveImageDesc(format, imageWidth, imageHeight, bitsPerPixel, generateMips, isPremultiplied);
        if (imageBytes->input.has_animation) {
            image->imageDesc_.imageFlags |= ImageFlags::FLAGS_ANIMATED_BIT;
        }
        image->imageBytes_ = BASE_NS::move(imageBytes);
        image->imageBytesLength_ = static_cast<size_t>(imageWidth * imageHeight * componentCount * bytesPerComponent);

        image->imageBuffer_ = SubImageDesc {
            0,           // uint32_t bufferOffset
            imageWidth,  // uint32_t bufferRowLength
            imageHeight, // uint32_t bufferImageHeight
            0,           // uint32_t mipLevel
            1,           // uint32_t layerCount
            imageWidth,  // uint32_t width
            imageHeight, // uint32_t height
            1,           // uint32_t depth
        };

        return ResultSuccess(BASE_NS::move(image));
    }

    // Actual WebP loading implementation.
    static IImageLoaderManager::LoadResult Load(array_view<const uint8_t> imageFileBytes, uint32_t loadFlags)
    {
        if (imageFileBytes.empty()) {
            return ResultFailure("Input data must not be null.");
        }

        auto config = std::make_unique<WebPDecoderConfig>(); // needs to allocated from heap as it's freed later
        if (!WebPInitDecoderConfig(config.get())) {
            return ResultFailure("Failed to initialize WebP decoder config");
        }

        int result = WebPGetFeatures(imageFileBytes.data(), imageFileBytes.size(), &config->input);

        if (result != VP8_STATUS_OK) {
            return ResultFailure("Loading image failed");
        }

        // Flip vertically if requested.
        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_FLIP_VERTICALLY_BIT) != 0) {
            config->options.flip = 1;
        }

        // Premultiply alpha is requested.
        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_PREMULTIPLY_ALPHA) != 0) {
            config->output.colorspace = MODE_rgbA;
        } else {
            config->output.colorspace = MODE_RGBA;
        }
        int width = config->input.width;
        int height = config->input.height;
        const int componentCount = 4;

        if (width <= 0 || height <= 0 || width > static_cast<int>(GS::MAX_IMAGE_DIM) ||
            height > static_cast<int>(GS::MAX_IMAGE_DIM)) {
            return ResultFailure("Image dimensions exceed allowed limit");
        }

        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_METADATA_ONLY) == 0) {
            result = WebPDecode(imageFileBytes.data(), imageFileBytes.size(), config.get());
            if (result != VP8_STATUS_OK) {
                return ResultFailure("Loading image failed");
            }
        }

        WebPImagePtr imageBytes = { config.release(), FreeWebPImage };

        // Success. Populate the image info and image data object.
        return CreateImage(BASE_NS::move(imageBytes), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            static_cast<uint32_t>(componentCount), loadFlags);
    }

protected:
    void Destroy() override
    {
        delete this;
    }

private:
    ImageDesc imageDesc_;
    SubImageDesc imageBuffer_;

    WebPImagePtr imageBytes_;
    size_t imageBytesLength_ = 0;
};

class WebPFrame final : public IImageContainer {
public:
    WebPFrame() : IImageContainer(), imageDesc_(), imageBuffer_() {}

    ~WebPFrame() override = default;

    using Ptr = BASE_NS::unique_ptr<WebPFrame, Deleter>;

    const ImageDesc& GetImageDesc() const override
    {
        return imageDesc_;
    }

    array_view<const uint8_t> GetData() const override
    {
        return array_view<const uint8_t>(static_cast<const uint8_t*>(imageBytes_.get()), imageBytesLength_);
    }

    array_view<const SubImageDesc> GetBufferImageCopies() const override
    {
        return array_view<const SubImageDesc>(&imageBuffer_, 1);
    }

    static constexpr Format ResolveFormat(uint32_t loadFlags, uint32_t componentCount, bool is16bpc)
    {
        Format format {};
        const bool forceLinear = (loadFlags & IImageLoaderManager::IMAGE_LOADER_FORCE_LINEAR_RGB_BIT) != 0;

        switch (componentCount) {
            case 1u:
                format = is16bpc ? Format::BASE_FORMAT_R16_UNORM
                                 : (forceLinear ? Format::BASE_FORMAT_R8_UNORM : Format::BASE_FORMAT_R8_SRGB);
                break;
            case 2u:
                format = is16bpc ? Format::BASE_FORMAT_R16G16_UNORM
                                 : (forceLinear ? Format::BASE_FORMAT_R8G8_UNORM : Format::BASE_FORMAT_R8G8_SRGB);
                break;
            case 3u:
                format = is16bpc ? Format::BASE_FORMAT_R16G16B16_UNORM
                                 : (forceLinear ? Format::BASE_FORMAT_R8G8B8_UNORM : Format::BASE_FORMAT_R8G8B8_SRGB);
                break;
            case 4u:
                format = is16bpc
                             ? Format::BASE_FORMAT_R16G16B16A16_UNORM
                             : (forceLinear ? Format::BASE_FORMAT_R8G8B8A8_UNORM : Format::BASE_FORMAT_R8G8B8A8_SRGB);
                break;
            default:
                format = Format::BASE_FORMAT_UNDEFINED;
                break;
        }

        return format;
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

    static IImageLoaderManager::LoadResult CreateFromBuffer(BASE_NS::unique_ptr<uint8_t[]> imageBytes,
        uint32_t imageWidth, uint32_t imageHeight, uint32_t componentCount, uint32_t loadFlags)
    {
        auto image = WebPFrame::Ptr(new WebPFrame);
        if (!image) {
            return ResultFailure("Loading image failed.");
        }

        // Premultiply alpha if requested.
        bool isPremultiplied = false;
        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_PREMULTIPLY_ALPHA) != 0) {
            isPremultiplied = true;
        }

        const Format format = ResolveFormat(loadFlags, componentCount, false);

        const bool generateMips = (loadFlags & IImageLoaderManager::IMAGE_LOADER_GENERATE_MIPS) != 0;
        const uint32_t bytesPerComponent = 1u;
        const uint32_t bitsPerPixel = bytesPerComponent * componentCount * 8u;

        image->imageDesc_ =
            ResolveImageDesc(format, imageWidth, imageHeight, bitsPerPixel, generateMips, isPremultiplied);
        image->imageDesc_.imageFlags |= ImageFlags::FLAGS_ANIMATED_BIT;
        image->imageBytes_ = BASE_NS::move(imageBytes);
        image->imageBytesLength_ = static_cast<size_t>(imageWidth * imageHeight * componentCount * bytesPerComponent);

        image->imageBuffer_ = SubImageDesc {
            0,           // uint32_t bufferOffset
            imageWidth,  // uint32_t bufferRowLength
            imageHeight, // uint32_t bufferImageHeight
            0,           // uint32_t mipLevel
            1,           // uint32_t layerCount
            imageWidth,  // uint32_t width
            imageHeight, // uint32_t height
            1,           // uint32_t depth
        };

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

    BASE_NS::unique_ptr<uint8_t> imageBytes_;
    size_t imageBytesLength_ = 0;
};

class WebPAnimatedImage final : public IAnimatedImage {
public:
    WebPAnimatedImage() : IAnimatedImage() {}
    ~WebPAnimatedImage() override = default;

    using Ptr = BASE_NS::unique_ptr<WebPAnimatedImage, Deleter>;

    void Restart() override
    {
        WebPAnimDecoderReset(dec_);
        frameNumber_ = 0;
    }

    uint32_t GetLoopCount() override
    {
        return anim_info_.loop_count;
    }

    uint32_t GetNumberOfFrames() override
    {
        return anim_info_.frame_count;
    }

    IAnimatedImage::AnimationFrame GetNextFrame() override
    {
        AnimationFrame frame { 0, 0, nullptr };

        if (!WebPAnimDecoderHasMoreFrames(dec_)) {
            WebPAnimDecoderReset(dec_);
            frameNumber_ = 0;
        }
        if (WebPAnimDecoderHasMoreFrames(dec_)) {
            uint8_t* frame_rgba;
            int timestamp;

            if (WebPAnimDecoderGetNext(dec_, &frame_rgba, &timestamp)) {
                frame.timestamp = timestamp >= 0 ? static_cast<uint64_t>(timestamp) : 0U;
                frame.frameNumber = frameNumber_++;

                if (anim_info_.canvas_width == 0 || anim_info_.canvas_height == 0 ||
                    anim_info_.canvas_width > static_cast<uint32_t>(GS::MAX_IMAGE_DIM) ||
                    anim_info_.canvas_height > static_cast<uint32_t>(GS::MAX_IMAGE_DIM)) {
                    return { 0, 0, nullptr };
                }
                size_t frame_size = static_cast<size_t>(anim_info_.canvas_width) *
                    static_cast<size_t>(anim_info_.canvas_height) * 4u;
                if (frame_size / 4u != static_cast<size_t>(anim_info_.canvas_width) *
                    static_cast<size_t>(anim_info_.canvas_height)) {
                    return { 0, 0, nullptr };
                }
                auto buffer = BASE_NS::make_unique<uint8_t[]>(frame_size);
                BASE_NS::CloneData(buffer.get(), frame_size, frame_rgba, frame_size);

                auto res = WebPFrame::CreateFromBuffer(
                    BASE_NS::move(buffer), anim_info_.canvas_width, anim_info_.canvas_height, 4, 0);
                if (!res.success) {
                    return {0, 0, nullptr};
                }
                frame.image = BASE_NS::move(res.image);
            }
        }
        return frame;
    }

    static IImageLoaderManager::LoadAnimatedResult Load(array_view<const uint8_t> imageFileBytes, uint32_t loadFlags)
    {
        auto img = WebPAnimatedImage::Ptr(new WebPAnimatedImage);

        WebPDataInit(&img->webp_data_);

        img->webp_data_.size = imageFileBytes.size();
        if (img->webp_data_.size == 0 || img->webp_data_.size > GS::MAX_WEBP_FILE_SIZE) {
            return ResultFailureAnimated("Invalid file size");
        }
        img->webp_data_.bytes = static_cast<uint8_t*>(WebPMalloc(img->webp_data_.size));
        if (!img->webp_data_.bytes) {
            return ResultFailureAnimated("WebPMalloc failed");
        }
        BASE_NS::CloneData(const_cast<uint8_t*>(img->webp_data_.bytes), imageFileBytes.size(), imageFileBytes.data(),
            imageFileBytes.size());

        img->dec_ = WebPAnimDecoderNew(&img->webp_data_, nullptr);
        if (!img->dec_) {
            return ResultFailureAnimated("WebPAnimDecoderNew failed");
        }

        if (!WebPAnimDecoderGetInfo(img->dec_, &img->anim_info_)) {
            return ResultFailureAnimated("Decoding image failed");
        }

        if ((loadFlags & IImageLoaderManager::IMAGE_LOADER_METADATA_ONLY) == 0) {
        }

        return ResultSuccessAnimated(BASE_NS::move(img));
    }

protected:
    void Destroy() override
    {
        if (dec_) {
            WebPAnimDecoderDelete(dec_);
            dec_ = nullptr;
        }
        WebPFree(const_cast<uint8_t*>(webp_data_.bytes));
        delete this;
    }

private:
    WebPData webp_data_;
    WebPAnimDecoder* dec_ { nullptr };
    WebPAnimInfo anim_info_;
    uint32_t frameNumber_ { 0 };
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
        unique_ptr<uint8_t[]> buffer = make_unique<uint8_t[]>(static_cast<size_t>(byteLength));
        const uint64_t read = file.Read(buffer.get(), byteLength);
        if (read != byteLength) {
            return ResultFailure("Reading file failed.");
        }

        return WebPImage::Load(array_view<const uint8_t>(buffer.get(), static_cast<size_t>(byteLength)), loadFlags);
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

    IImageLoaderManager::LoadAnimatedResult LoadAnimatedImage(IFile& file, uint32_t loadFlags) override
    {
        const uint64_t byteLength = file.GetLength();

        if (byteLength == 0 || byteLength > GS::MAX_WEBP_FILE_SIZE) {
            return ResultFailureAnimated("Invalid file size");
        }

        // Read the file to a buffer. NOTE SHOULD IMPLEMENT INCREMENTAL FILE INTERFACE
        unique_ptr<uint8_t[]> buffer = make_unique<uint8_t[]>(static_cast<size_t>(byteLength));
        const uint64_t read = file.Read(buffer.get(), byteLength);
        if (read != byteLength) {
            return ResultFailureAnimated("Reading file failed.");
        }
        return WebPAnimatedImage::Load(
            array_view<const uint8_t>(buffer.get(), static_cast<size_t>(byteLength)), loadFlags);
    }

    IImageLoaderManager::LoadAnimatedResult LoadAnimatedImage(
        array_view<const uint8_t> imageFileBytes, uint32_t loadFlags) override
    {
        return WebPAnimatedImage::Load(imageFileBytes, loadFlags);
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
