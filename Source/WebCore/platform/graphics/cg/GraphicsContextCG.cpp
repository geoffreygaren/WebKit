/*
 * Copyright (C) 2003-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Eric Seidel <eric@webkit.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GraphicsContextCG.h"

#if USE(CG)

#include "AffineTransform.h"
#include "CGSubimageCacheWithTimer.h"
#include "CGUtilities.h"
#include "DisplayListRecorder.h"
#include "FloatConversion.h"
#include "Gradient.h"
#include "ImageBuffer.h"
#include "ImageOrientation.h"
#include "Logging.h"
#include "Path.h"
#include "PathCG.h"
#include "Pattern.h"
#include "ShadowBlur.h"
#include "Timer.h"
#include <pal/spi/cg/CoreGraphicsSPI.h>
#include <wtf/MathExtras.h>
#include <wtf/RetainPtr.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/URL.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GraphicsContextCG);

static void setCGFillColor(CGContextRef context, const Color& color, const DestinationColorSpace& colorSpace)
{
    CGContextSetFillColorWithColor(context, cachedSDRCGColorForColorspace(color, colorSpace).get());
}

inline CGAffineTransform getUserToBaseCTM(CGContextRef context)
{
    return CGAffineTransformConcat(CGContextGetCTM(context), CGAffineTransformInvert(CGContextGetBaseCTM(context)));
}

static InterpolationQuality coreInterpolationQuality(CGContextRef context)
{
    switch (CGContextGetInterpolationQuality(context)) {
    case kCGInterpolationDefault:
        return InterpolationQuality::Default;
    case kCGInterpolationNone:
        return InterpolationQuality::DoNotInterpolate;
    case kCGInterpolationLow:
        return InterpolationQuality::Low;
    case kCGInterpolationMedium:
        return InterpolationQuality::Medium;
    case kCGInterpolationHigh:
        return InterpolationQuality::High;
    }
    return InterpolationQuality::Default;
}

static CGTextDrawingMode cgTextDrawingMode(TextDrawingModeFlags mode)
{
    bool fill = mode.contains(TextDrawingMode::Fill);
    bool stroke = mode.contains(TextDrawingMode::Stroke);
    if (fill && stroke)
        return kCGTextFillStroke;
    if (fill)
        return kCGTextFill;
    return kCGTextStroke;
}

static CGBlendMode selectCGBlendMode(CompositeOperator compositeOperator, BlendMode blendMode)
{
    switch (blendMode) {
    case BlendMode::Normal:
        switch (compositeOperator) {
        case CompositeOperator::Clear:
            return kCGBlendModeClear;
        case CompositeOperator::Copy:
            return kCGBlendModeCopy;
        case CompositeOperator::SourceOver:
            return kCGBlendModeNormal;
        case CompositeOperator::SourceIn:
            return kCGBlendModeSourceIn;
        case CompositeOperator::SourceOut:
            return kCGBlendModeSourceOut;
        case CompositeOperator::SourceAtop:
            return kCGBlendModeSourceAtop;
        case CompositeOperator::DestinationOver:
            return kCGBlendModeDestinationOver;
        case CompositeOperator::DestinationIn:
            return kCGBlendModeDestinationIn;
        case CompositeOperator::DestinationOut:
            return kCGBlendModeDestinationOut;
        case CompositeOperator::DestinationAtop:
            return kCGBlendModeDestinationAtop;
        case CompositeOperator::XOR:
            return kCGBlendModeXOR;
        case CompositeOperator::PlusDarker:
            return kCGBlendModePlusDarker;
        case CompositeOperator::PlusLighter:
            return kCGBlendModePlusLighter;
        case CompositeOperator::Difference:
            return kCGBlendModeDifference;
        }
        break;
    case BlendMode::Multiply:
        return kCGBlendModeMultiply;
    case BlendMode::Screen:
        return kCGBlendModeScreen;
    case BlendMode::Overlay:
        return kCGBlendModeOverlay;
    case BlendMode::Darken:
        return kCGBlendModeDarken;
    case BlendMode::Lighten:
        return kCGBlendModeLighten;
    case BlendMode::ColorDodge:
        return kCGBlendModeColorDodge;
    case BlendMode::ColorBurn:
        return kCGBlendModeColorBurn;
    case BlendMode::HardLight:
        return kCGBlendModeHardLight;
    case BlendMode::SoftLight:
        return kCGBlendModeSoftLight;
    case BlendMode::Difference:
        return kCGBlendModeDifference;
    case BlendMode::Exclusion:
        return kCGBlendModeExclusion;
    case BlendMode::Hue:
        return kCGBlendModeHue;
    case BlendMode::Saturation:
        return kCGBlendModeSaturation;
    case BlendMode::Color:
        return kCGBlendModeColor;
    case BlendMode::Luminosity:
        return kCGBlendModeLuminosity;
    case BlendMode::PlusDarker:
        return kCGBlendModePlusDarker;
    case BlendMode::PlusLighter:
        return kCGBlendModePlusLighter;
    }

    return kCGBlendModeNormal;
}

static void setCGBlendMode(CGContextRef context, CompositeOperator op, BlendMode blendMode)
{
    CGContextSetBlendMode(context, selectCGBlendMode(op, blendMode));
}

static void setCGContextPath(CGContextRef context, const Path& path)
{
    CGContextBeginPath(context);
    addToCGContextPath(context, path);
}

static void drawPathWithCGContext(CGContextRef context, CGPathDrawingMode drawingMode, const Path& path)
{
    CGContextDrawPathDirect(context, drawingMode, path.platformPath(), nullptr);
}

static RenderingMode renderingModeForCGContext(CGContextRef cgContext, GraphicsContextCG::CGContextSource source)
{
    if (!cgContext)
        return RenderingMode::Unaccelerated;
    auto type = CGContextGetType(cgContext);
    if (type == kCGContextTypeIOSurface || (source == GraphicsContextCG::CGContextFromCALayer && type == kCGContextTypeUnknown))
        return RenderingMode::Accelerated;
    if (type == kCGContextTypePDF)
        return RenderingMode::PDFDocument;
    return RenderingMode::Unaccelerated;
}

static GraphicsContext::IsDeferred isDeferredForCGContext(CGContextRef cgContext)
{
    if (!cgContext || CGContextGetType(cgContext) == kCGContextTypeBitmap)
        return GraphicsContext::IsDeferred::No;
    // Other CGContexts are deferred (iosurface, display list) or potentially deferred.
    return GraphicsContext::IsDeferred::Yes;
}

GraphicsContextCG::GraphicsContextCG(CGContextRef cgContext, CGContextSource source, std::optional<RenderingMode> knownRenderingMode)
    : GraphicsContext(isDeferredForCGContext(cgContext), GraphicsContextState::basicChangeFlags, coreInterpolationQuality(cgContext))
    , m_cgContext(cgContext)
    , m_renderingMode(knownRenderingMode.value_or(renderingModeForCGContext(cgContext, source)))
    , m_isLayerCGContext(source == GraphicsContextCG::CGContextFromCALayer)
{
    if (!cgContext)
        return;
    // Make sure the context starts in sync with our state.
    didUpdateState(m_state);
}

GraphicsContextCG::~GraphicsContextCG() = default;

bool GraphicsContextCG::hasPlatformContext() const
{
    return true;
}

CGContextRef GraphicsContextCG::contextForState() const
{
    ASSERT(m_cgContext);
    return m_cgContext.get();
}

const DestinationColorSpace& GraphicsContextCG::colorSpace() const
{
    if (m_colorSpace)
        return *m_colorSpace;

    RetainPtr context = platformContext();
    RetainPtr<CGColorSpaceRef> colorSpace;

    // FIXME: Need to handle kCGContextTypePDF.
    if (CGContextGetType(context.get()) == kCGContextTypeIOSurface)
        colorSpace = CGIOSurfaceContextGetColorSpace(context.get());
    else if (CGContextGetType(context.get()) == kCGContextTypeBitmap)
        colorSpace = CGBitmapContextGetColorSpace(context.get());
    else
        colorSpace = adoptCF(CGContextCopyDeviceColorSpace(context.get()));

    // FIXME: Need to ASSERT(colorSpace). For now fall back to sRGB if colorSpace is nil.
    m_colorSpace = colorSpace ? DestinationColorSpace(colorSpace) : DestinationColorSpace::SRGB();
    return *m_colorSpace;
}

void GraphicsContextCG::save(GraphicsContextState::Purpose purpose)
{
    GraphicsContext::save(purpose);
    CGContextSaveGState(contextForState());
}

void GraphicsContextCG::restore(GraphicsContextState::Purpose purpose)
{
    if (!stackSize())
        return;

    GraphicsContext::restore(purpose);
    CGContextRestoreGState(contextForState());
    m_userToDeviceTransformKnownToBeIdentity = false;
}

void GraphicsContextCG::drawNativeImage(NativeImage& nativeImage, const FloatRect& destRect, const FloatRect& srcRect, ImagePaintingOptions options)
{
    auto image = nativeImage.platformImage();
    if (!image)
        return;
    auto imageSize = nativeImage.size();
    if (options.orientation().usesWidthAsHeight())
        imageSize = imageSize.transposedSize();
    auto imageRect = FloatRect { { }, imageSize };
    auto normalizedSrcRect = normalizeRect(srcRect);
    auto normalizedDestRect = normalizeRect(destRect);
    if (!imageRect.intersects(normalizedSrcRect))
        return;

#if !LOG_DISABLED
    MonotonicTime startTime = MonotonicTime::now();
#endif

    auto shouldUseSubimage = [](CGInterpolationQuality interpolationQuality, const FloatRect& destRect, const FloatRect& srcRect, const AffineTransform& transform) -> bool {
        if (interpolationQuality == kCGInterpolationNone)
            return false;
        if (transform.isRotateOrShear())
            return true;
        auto xScale = destRect.width() * transform.xScale() / srcRect.width();
        auto yScale = destRect.height() * transform.yScale() / srcRect.height();
        return !WTF::areEssentiallyEqual(xScale, yScale) || xScale > 1;
    };

    auto getSubimage = [](CGImageRef image, const FloatSize& imageSize, const FloatRect& subimageRect, ImagePaintingOptions options) -> RetainPtr<CGImageRef> {
        auto physicalSubimageRect = subimageRect;

        if (options.orientation() != ImageOrientation::Orientation::None) {
            // subimageRect is in logical coordinates. getSubimage() deals with none-oriented
            // image. We need to convert subimageRect to physical image coordinates.
            if (auto transform = options.orientation().transformFromDefault(imageSize).inverse())
                physicalSubimageRect = transform.value().mapRect(physicalSubimageRect);
        }

#if CACHE_SUBIMAGES
        if (!(CGImageGetCachingFlags(image) & kCGImageCachingTransient))
            return CGSubimageCacheWithTimer::getSubimage(image, physicalSubimageRect);
#endif
        return adoptCF(CGImageCreateWithImageInRect(image, physicalSubimageRect));
    };

#if HAVE(SUPPORT_HDR_DISPLAY_APIS)
    auto setCGDynamicRangeLimitForImage = [](CGContextRef context, CGImageRef image, float dynamicRangeLimit) {
        float edrStrength = dynamicRangeLimit == 1.0 ? 1 : 0;
        float cdrStrength = dynamicRangeLimit == 0.5 ? 1 : 0;
        unsigned averageLightLevel = CGImageGetContentAverageLightLevelNits(image);

        RetainPtr edrStrengthNumber = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &edrStrength));
        RetainPtr cdrStrengthNumber = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &cdrStrength));
        RetainPtr averageLightLevelNumber = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &averageLightLevel));

        CFTypeRef toneMappingKeys[] = { kCGContentEDRStrength, kCGContentAverageLightLevel, kCGConstrainedDynamicRange };
        CFTypeRef toneMappingValues[] = { edrStrengthNumber.get(), averageLightLevelNumber.get(), cdrStrengthNumber.get() };

        RetainPtr toneMappingOptions = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, toneMappingKeys, toneMappingValues, sizeof(toneMappingKeys) / sizeof(toneMappingKeys[0]), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

        CGContentToneMappingInfo toneMappingInfo = { kCGToneMappingReferenceWhiteBased, toneMappingOptions.get() };
        CGContextSetContentToneMappingInfo(context, toneMappingInfo);
    };
#endif

    RetainPtr context = platformContext();
    CGContextStateSaver stateSaver(context.get(), false);
    auto transform = CGContextGetCTM(context.get());

    auto subImage = image;

    auto adjustedDestRect = normalizedDestRect;

    if (normalizedSrcRect != imageRect) {
        CGInterpolationQuality interpolationQuality = CGContextGetInterpolationQuality(context.get());
        auto scale = normalizedDestRect.size() / normalizedSrcRect.size();

        if (shouldUseSubimage(interpolationQuality, normalizedDestRect, normalizedSrcRect, transform)) {
            auto subimageRect = enclosingIntRect(normalizedSrcRect);

            // When the image is scaled using high-quality interpolation, we create a temporary CGImage
            // containing only the portion we want to display. We need to do this because high-quality
            // interpolation smoothes sharp edges, causing pixels from outside the source rect to bleed
            // into the destination rect. See <rdar://problem/6112909>.
            subImage = getSubimage(subImage.get(), imageSize, subimageRect, options);

            auto subPixelPadding = normalizedSrcRect.location() - subimageRect.location();
            adjustedDestRect = { adjustedDestRect.location() - subPixelPadding * scale, subimageRect.size() * scale };
        } else {
            // If the source rect is a subportion of the image, then we compute an inflated destination rect
            // that will hold the entire image and then set a clip to the portion that we want to display.
            adjustedDestRect = { adjustedDestRect.location() - toFloatSize(normalizedSrcRect.location()) * scale, imageSize * scale };
        }

        if (!normalizedDestRect.contains(adjustedDestRect)) {
            stateSaver.save();
            CGContextClipToRect(context.get(), normalizedDestRect);
        }
    }

#if PLATFORM(IOS_FAMILY)
    bool wasAntialiased = CGContextGetShouldAntialias(context.get());
    // Anti-aliasing is on by default on the iPhone. Need to turn it off when drawing images.
    CGContextSetShouldAntialias(context.get(), false);

    // Align to pixel boundaries
    adjustedDestRect = roundToDevicePixels(adjustedDestRect);
#endif

    auto oldCompositeOperator = compositeOperation();
    auto oldBlendMode = blendMode();
    setCGBlendMode(context.get(), options.compositeOperator(), options.blendMode());

#if HAVE(SUPPORT_HDR_DISPLAY_APIS)
    auto oldHeadroom = CGContextGetEDRTargetHeadroom(context.get());
    auto oldToneMappingInfo = CGContextGetContentToneMappingInfo(context.get());

    auto headroom = options.headroom();
    if (headroom == Headroom::FromImage)
        headroom = nativeImage.headroom();
    if (m_maxEDRHeadroom)
        headroom = Headroom(std::min<float>(headroom, *m_maxEDRHeadroom));

    if (nativeImage.headroom() > headroom) {
        LOG_WITH_STREAM(HDR, stream << "GraphicsContextCG::drawNativeImage setEDRTargetHeadroom " << headroom << " max(" << m_maxEDRHeadroom << ")");
        CGContextSetEDRTargetHeadroom(context.get(), headroom);
    }

    if (options.dynamicRangeLimit() == PlatformDynamicRangeLimit::standard() && options.drawsHDRContent() == DrawsHDRContent::Yes)
        setCGDynamicRangeLimitForImage(context.get(), subImage.get(), options.dynamicRangeLimit().value());
#endif

    // Make the origin be at adjustedDestRect.location()
    CGContextTranslateCTM(context.get(), adjustedDestRect.x(), adjustedDestRect.y());
    adjustedDestRect.setLocation(FloatPoint::zero());

    if (options.orientation() != ImageOrientation::Orientation::None) {
        CGContextConcatCTM(context.get(), options.orientation().transformFromDefault(adjustedDestRect.size()));

        // The destination rect will have its width and height already reversed for the orientation of
        // the image, as it was needed for page layout, so we need to reverse it back here.
        if (options.orientation().usesWidthAsHeight())
            adjustedDestRect = adjustedDestRect.transposedRect();
    }

    // Flip the coords.
    CGContextTranslateCTM(context.get(), 0, adjustedDestRect.height());
    CGContextScaleCTM(context.get(), 1, -1);

    // Draw the image.
    CGContextDrawImage(context.get(), adjustedDestRect, subImage.get());

    if (!stateSaver.didSave()) {
        CGContextSetCTM(context.get(), transform);
#if PLATFORM(IOS_FAMILY)
        CGContextSetShouldAntialias(context.get(), wasAntialiased);
#endif
        setCGBlendMode(context.get(), oldCompositeOperator, oldBlendMode);
#if HAVE(SUPPORT_HDR_DISPLAY_APIS)
        CGContextSetContentToneMappingInfo(context.get(), oldToneMappingInfo);
        CGContextSetEDRTargetHeadroom(context.get(), oldHeadroom);
#endif
    }

    LOG_WITH_STREAM(Images, stream << "GraphicsContextCG::drawNativeImage " << image.get() << " size " << imageSize << " into " << destRect << " took " << (MonotonicTime::now() - startTime).milliseconds() << "ms");
}

static void drawPatternCallback(void* info, CGContextRef context)
{
    CGImageRef image = (CGImageRef)info;
    auto rect = cgRoundToDevicePixels(CGContextGetUserSpaceToDeviceSpaceTransform(context), cgImageRect(image));
    CGContextDrawImage(context, rect, image);
}

static void patternReleaseCallback(void* info)
{
    callOnMainThread([image = adoptCF(static_cast<CGImageRef>(info))] { });
}

void GraphicsContextCG::drawPattern(NativeImage& nativeImage, const FloatRect& destRect, const FloatRect& tileRect, const AffineTransform& patternTransform, const FloatPoint& phase, const FloatSize& spacing, ImagePaintingOptions options)
{
    if (!patternTransform.isInvertible())
        return;

    auto image = nativeImage.platformImage();
    auto imageSize = nativeImage.size();

    RetainPtr context = platformContext();
    CGContextStateSaver stateSaver(context.get());
    CGContextClipToRect(context.get(), destRect);

    setCGBlendMode(context.get(), options.compositeOperator(), options.blendMode());

    CGContextTranslateCTM(context.get(), destRect.x(), destRect.y() + destRect.height());
    CGContextScaleCTM(context.get(), 1, -1);

    // Compute the scaled tile size.
    float scaledTileHeight = tileRect.height() * narrowPrecisionToFloat(patternTransform.d());

    // We have to adjust the phase to deal with the fact we're in Cartesian space now (with the bottom left corner of destRect being
    // the origin).
    float adjustedX = phase.x() - destRect.x() + tileRect.x() * narrowPrecisionToFloat(patternTransform.a()); // We translated the context so that destRect.x() is the origin, so subtract it out.
    float adjustedY = destRect.height() - (phase.y() - destRect.y() + tileRect.y() * narrowPrecisionToFloat(patternTransform.d()) + scaledTileHeight);

    float h = CGImageGetHeight(image.get());

    RetainPtr<CGImageRef> subImage;
    if (tileRect.size() == imageSize)
        subImage = image;
    else {
        // Copying a sub-image out of a partially-decoded image stops the decoding of the original image. It should never happen
        // because sub-images are only used for border-image, which only renders when the image is fully decoded.
        ASSERT(h == imageSize.height());
        subImage = adoptCF(CGImageCreateWithImageInRect(image.get(), tileRect));
    }

    // If we need to paint gaps between tiles because we have a partially loaded image or non-zero spacing,
    // fall back to the less efficient CGPattern-based mechanism.
    float scaledTileWidth = tileRect.width() * narrowPrecisionToFloat(patternTransform.a());
    float w = CGImageGetWidth(image.get());
    if (w == imageSize.width() && h == imageSize.height() && !spacing.width() && !spacing.height()) {
        // FIXME: CG seems to snap the images to integral sizes. When we care (e.g. with border-image-repeat: round),
        // we should tile all but the last, and stretch the last image to fit.
        CGContextDrawTiledImage(context.get(), FloatRect(adjustedX, adjustedY, scaledTileWidth, scaledTileHeight), subImage.get());
    } else {
        static const CGPatternCallbacks patternCallbacks = { 0, drawPatternCallback, patternReleaseCallback };
        CGAffineTransform matrix = CGAffineTransformMake(narrowPrecisionToCGFloat(patternTransform.a()), 0, 0, narrowPrecisionToCGFloat(patternTransform.d()), adjustedX, adjustedY);
        matrix = CGAffineTransformConcat(matrix, CGContextGetCTM(context.get()));
        // The top of a partially-decoded image is drawn at the bottom of the tile. Map it to the top.
        matrix = CGAffineTransformTranslate(matrix, 0, imageSize.height() - h);
        RetainPtr platformImage = CGImageRetain(subImage.get());
        RetainPtr<CGPatternRef> pattern = adoptCF(CGPatternCreate(platformImage.get(), CGRectMake(0, 0, tileRect.width(), tileRect.height()), matrix,
            tileRect.width() + spacing.width() * (1 / narrowPrecisionToFloat(patternTransform.a())),
            tileRect.height() + spacing.height() * (1 / narrowPrecisionToFloat(patternTransform.d())),
            kCGPatternTilingConstantSpacing, true, &patternCallbacks));

        if (!pattern)
            return;

        RetainPtr<CGColorSpaceRef> patternSpace = adoptCF(CGColorSpaceCreatePattern(nullptr));

        CGFloat alpha = 1;
        RetainPtr<CGColorRef> color = adoptCF(CGColorCreateWithPattern(patternSpace.get(), pattern.get(), &alpha));
        CGContextSetFillColorSpace(context.get(), patternSpace.get());

        CGContextSetBaseCTM(context.get(), CGAffineTransformIdentity);
        CGContextSetPatternPhase(context.get(), CGSizeZero);

        CGContextSetFillColorWithColor(context.get(), color.get());
        CGContextFillRect(context.get(), CGContextGetClipBoundingBox(context.get())); // FIXME: we know the clip; we set it above.
    }
}

// Draws a filled rectangle with a stroked border.
void GraphicsContextCG::drawRect(const FloatRect& rect, float borderThickness)
{
    // FIXME: this function does not handle patterns and gradients like drawPath does, it probably should.
    ASSERT(!rect.isEmpty());

    RetainPtr context = platformContext();

    CGContextFillRect(context.get(), rect);

    if (strokeStyle() != StrokeStyle::NoStroke) {
        // We do a fill of four rects to simulate the stroke of a border.
        Color oldFillColor = fillColor();
        if (oldFillColor != strokeColor())
            setCGFillColor(context.get(), strokeColor(), colorSpace());
        CGRect rects[4] = {
            FloatRect(rect.x(), rect.y(), rect.width(), borderThickness),
            FloatRect(rect.x(), rect.maxY() - borderThickness, rect.width(), borderThickness),
            FloatRect(rect.x(), rect.y() + borderThickness, borderThickness, rect.height() - 2 * borderThickness),
            FloatRect(rect.maxX() - borderThickness, rect.y() + borderThickness, borderThickness, rect.height() - 2 * borderThickness)
        };
        CGContextFillRects(context.get(), rects, 4);
        if (oldFillColor != strokeColor())
            setCGFillColor(context.get(), oldFillColor, colorSpace());
    }
}

// This is only used to draw borders.
void GraphicsContextCG::drawLine(const FloatPoint& point1, const FloatPoint& point2)
{
    if (strokeStyle() == StrokeStyle::NoStroke)
        return;

    float thickness = strokeThickness();
    bool isVerticalLine = (point1.x() + thickness == point2.x());
    float strokeWidth = isVerticalLine ? point2.y() - point1.y() : point2.x() - point1.x();
    if (!thickness || !strokeWidth)
        return;

    RetainPtr context = platformContext();

    StrokeStyle strokeStyle = this->strokeStyle();
    float cornerWidth = 0;
    bool drawsDashedLine = strokeStyle == StrokeStyle::DottedStroke || strokeStyle == StrokeStyle::DashedStroke;

    CGContextStateSaver stateSaver(context.get(), drawsDashedLine);
    if (drawsDashedLine) {
        // Figure out end points to ensure we always paint corners.
        cornerWidth = dashedLineCornerWidthForStrokeWidth(strokeWidth);
        setCGFillColor(context.get(), strokeColor(), colorSpace());
        if (isVerticalLine) {
            CGContextFillRect(context.get(), FloatRect(point1.x(), point1.y(), thickness, cornerWidth));
            CGContextFillRect(context.get(), FloatRect(point1.x(), point2.y() - cornerWidth, thickness, cornerWidth));
        } else {
            CGContextFillRect(context.get(), FloatRect(point1.x(), point1.y(), cornerWidth, thickness));
            CGContextFillRect(context.get(), FloatRect(point2.x() - cornerWidth, point1.y(), cornerWidth, thickness));
        }
        strokeWidth -= 2 * cornerWidth;
        float patternWidth = dashedLinePatternWidthForStrokeWidth(strokeWidth);
        // Check if corner drawing sufficiently covers the line.
        if (strokeWidth <= patternWidth + 1)
            return;

        float patternOffset = dashedLinePatternOffsetForPatternAndStrokeWidth(patternWidth, strokeWidth);
        const CGFloat dashedLine[2] = { static_cast<CGFloat>(patternWidth), static_cast<CGFloat>(patternWidth) };
        CGContextSetLineDash(context.get(), patternOffset, dashedLine, 2);
    }

    auto centeredPoints = centerLineAndCutOffCorners(isVerticalLine, cornerWidth, point1, point2);
    auto p1 = centeredPoints[0];
    auto p2 = centeredPoints[1];

    if (shouldAntialias()) {
#if PLATFORM(IOS_FAMILY)
        // Force antialiasing on for line patterns as they don't look good with it turned off (<rdar://problem/5459772>).
        CGContextSetShouldAntialias(context.get(), strokeStyle == StrokeStyle::DottedStroke || strokeStyle == StrokeStyle::DashedStroke);
#else
        CGContextSetShouldAntialias(context.get(), false);
#endif
    }
    CGContextBeginPath(context.get());
    CGContextMoveToPoint(context.get(), p1.x(), p1.y());
    CGContextAddLineToPoint(context.get(), p2.x(), p2.y());
    CGContextStrokePath(context.get());
    if (shouldAntialias())
        CGContextSetShouldAntialias(context.get(), true);
}

void GraphicsContextCG::drawEllipse(const FloatRect& rect)
{
    Path path;
    path.addEllipseInRect(rect);
    drawPath(path);
}

void GraphicsContextCG::applyStrokePattern()
{
    RefPtr strokePattern = this->strokePattern();
    if (!strokePattern)
        return;

    RetainPtr cgContext = platformContext();
    AffineTransform userToBaseCTM = AffineTransform(getUserToBaseCTM(cgContext.get()));

    auto platformPattern = strokePattern->createPlatformPattern(userToBaseCTM);
    if (!platformPattern)
        return;

    RetainPtr<CGColorSpaceRef> patternSpace = adoptCF(CGColorSpaceCreatePattern(0));
    CGContextSetStrokeColorSpace(cgContext.get(), patternSpace.get());

    const CGFloat patternAlpha = 1;
    CGContextSetStrokePattern(cgContext.get(), platformPattern.get(), &patternAlpha);
}

void GraphicsContextCG::applyFillPattern()
{
    RefPtr fillPattern = this->fillPattern();
    if (!fillPattern)
        return;

    RetainPtr cgContext = platformContext();
    AffineTransform userToBaseCTM = AffineTransform(getUserToBaseCTM(cgContext.get()));

    auto platformPattern = fillPattern->createPlatformPattern(userToBaseCTM);
    if (!platformPattern)
        return;

    RetainPtr<CGColorSpaceRef> patternSpace = adoptCF(CGColorSpaceCreatePattern(nullptr));
    CGContextSetFillColorSpace(cgContext.get(), patternSpace.get());

    const CGFloat patternAlpha = 1;
    CGContextSetFillPattern(cgContext.get(), platformPattern.get(), &patternAlpha);
}

static inline bool calculateDrawingMode(const GraphicsContext& context, CGPathDrawingMode& mode)
{
    bool shouldFill = context.fillBrush().isVisible();
    bool shouldStroke = context.strokeBrush().isVisible() || (context.strokeStyle() != StrokeStyle::NoStroke);
    bool useEOFill = context.fillRule() == WindRule::EvenOdd;

    if (shouldFill) {
        if (shouldStroke) {
            if (useEOFill)
                mode = kCGPathEOFillStroke;
            else
                mode = kCGPathFillStroke;
        } else { // fill, no stroke
            if (useEOFill)
                mode = kCGPathEOFill;
            else
                mode = kCGPathFill;
        }
    } else {
        // Setting mode to kCGPathStroke even if shouldStroke is false. In that case, we return false and mode will not be used,
        // but the compiler will not complain about an uninitialized variable.
        mode = kCGPathStroke;
    }

    return shouldFill || shouldStroke;
}

void GraphicsContextCG::drawPath(const Path& path)
{
    if (path.isEmpty())
        return;

    RetainPtr context = platformContext();

    if (fillGradient() || strokeGradient()) {
        // We don't have any optimized way to fill & stroke a path using gradients
        // FIXME: Be smarter about this.
        fillPath(path);
        strokePath(path);
        return;
    }

    if (fillPattern())
        applyFillPattern();
    if (strokePattern())
        applyStrokePattern();

    CGPathDrawingMode drawingMode;
    if (calculateDrawingMode(*this, drawingMode))
        drawPathWithCGContext(context.get(), drawingMode, path);
}

void GraphicsContextCG::fillPath(const Path& path)
{
    if (path.isEmpty())
        return;

    RetainPtr context = platformContext();

    if (RefPtr fillGradient = this->fillGradient()) {
        if (hasDropShadow()) {
            FloatRect rect = path.fastBoundingRect();
            FloatSize layerSize = getCTM().mapSize(rect.size());

            auto layer = adoptCF(CGLayerCreateWithContext(context.get(), layerSize, 0));
            RetainPtr layerContext = CGLayerGetContext(layer.get());

            CGContextScaleCTM(layerContext.get(), layerSize.width() / rect.width(), layerSize.height() / rect.height());
            CGContextTranslateCTM(layerContext.get(), -rect.x(), -rect.y());
            setCGContextPath(layerContext.get(), path);
            CGContextConcatCTM(layerContext.get(), fillGradientSpaceTransform());

            if (fillRule() == WindRule::EvenOdd)
                CGContextEOClip(layerContext.get());
            else
                CGContextClip(layerContext.get());

            fillGradient->paint(layerContext.get());
            CGContextDrawLayerInRect(context.get(), rect, layer.get());
        } else {
            setCGContextPath(context.get(), path);
            CGContextStateSaver stateSaver(context.get());
            CGContextConcatCTM(context.get(), fillGradientSpaceTransform());

            if (fillRule() == WindRule::EvenOdd)
                CGContextEOClip(context.get());
            else
                CGContextClip(context.get());

            fillGradient->paint(*this);
        }

        return;
    }

    if (fillPattern())
        applyFillPattern();

    drawPathWithCGContext(context.get(), fillRule() == WindRule::EvenOdd ? kCGPathEOFill : kCGPathFill, path);
}

void GraphicsContextCG::strokePath(const Path& path)
{
    if (path.isEmpty())
        return;

    RetainPtr context = platformContext();

    if (RefPtr strokeGradient = this->strokeGradient()) {
        if (hasDropShadow()) {
            FloatRect rect = path.fastBoundingRect();
            float lineWidth = strokeThickness();
            float doubleLineWidth = lineWidth * 2;
            float adjustedWidth = ceilf(rect.width() + doubleLineWidth);
            float adjustedHeight = ceilf(rect.height() + doubleLineWidth);

            FloatSize layerSize = getCTM().mapSize(FloatSize(adjustedWidth, adjustedHeight));

            auto layer = adoptCF(CGLayerCreateWithContext(context.get(), layerSize, 0));
            RetainPtr layerContext = CGLayerGetContext(layer.get());
            CGContextSetLineWidth(layerContext.get(), lineWidth);

            // Compensate for the line width, otherwise the layer's top-left corner would be
            // aligned with the rect's top-left corner. This would result in leaving pixels out of
            // the layer on the left and top sides.
            float translationX = lineWidth - rect.x();
            float translationY = lineWidth - rect.y();
            CGContextScaleCTM(layerContext.get(), layerSize.width() / adjustedWidth, layerSize.height() / adjustedHeight);
            CGContextTranslateCTM(layerContext.get(), translationX, translationY);

            setCGContextPath(layerContext.get(), path);
            CGContextReplacePathWithStrokedPath(layerContext.get());
            CGContextClip(layerContext.get());
            CGContextConcatCTM(layerContext.get(), strokeGradientSpaceTransform());
            strokeGradient->paint(layerContext.get());

            float destinationX = roundf(rect.x() - lineWidth);
            float destinationY = roundf(rect.y() - lineWidth);
            CGContextDrawLayerInRect(context.get(), CGRectMake(destinationX, destinationY, adjustedWidth, adjustedHeight), layer.get());
        } else {
            CGContextStateSaver stateSaver(context.get());
            setCGContextPath(context.get(), path);
            CGContextReplacePathWithStrokedPath(context.get());
            CGContextClip(context.get());
            CGContextConcatCTM(context.get(), strokeGradientSpaceTransform());
            strokeGradient->paint(*this);
        }
        return;
    }

    if (strokePattern())
        applyStrokePattern();

    if (auto line = path.singleDataLine()) {
        CGPoint cgPoints[2] { line->start(), line->end() };
        CGContextStrokeLineSegments(context.get(), cgPoints, 2);
        return;
    }

    drawPathWithCGContext(context.get(), kCGPathStroke, path);
}

void GraphicsContextCG::fillRect(const FloatRect& rect, RequiresClipToRect requiresClipToRect)
{
    RetainPtr context = platformContext();

    if (RefPtr fillGradient = this->fillGradient()) {
        fillRect(rect, *fillGradient, fillGradientSpaceTransform(), requiresClipToRect);
        return;
    }

    if (fillPattern())
        applyFillPattern();

    bool drawOwnShadow = canUseShadowBlur();
    CGContextStateSaver stateSaver(context.get(), drawOwnShadow);
    if (drawOwnShadow) {
        clearCGDropShadow();

        const auto shadow = dropShadow();
        ASSERT(shadow);

        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawRectShadow(*this, FloatRoundedRect(rect));
    }

    CGContextFillRect(context.get(), rect);
}

void GraphicsContextCG::fillRect(const FloatRect& rect, Gradient& gradient, const AffineTransform& gradientSpaceTransform, RequiresClipToRect requiresClipToRect)
{
    RetainPtr context = platformContext();

    CGContextStateSaver stateSaver(context.get());
    if (hasDropShadow()) {
        FloatSize layerSize = getCTM().mapSize(rect.size());

        auto layer = adoptCF(CGLayerCreateWithContext(context.get(), layerSize, 0));
        RetainPtr layerContext = CGLayerGetContext(layer.get());

        CGContextScaleCTM(layerContext.get(), layerSize.width() / rect.width(), layerSize.height() / rect.height());
        CGContextTranslateCTM(layerContext.get(), -rect.x(), -rect.y());
        CGContextAddRect(layerContext.get(), rect);
        CGContextClip(layerContext.get());

        CGContextConcatCTM(layerContext.get(), gradientSpaceTransform);
        gradient.paint(layerContext.get());
        CGContextDrawLayerInRect(context.get(), rect, layer.get());
    } else {
        if (requiresClipToRect == RequiresClipToRect::Yes)
            CGContextClipToRect(context.get(), rect);

        CGContextConcatCTM(context.get(), gradientSpaceTransform);
        gradient.paint(*this);
    }
}

void GraphicsContextCG::fillRect(const FloatRect& rect, const Color& color)
{
    RetainPtr context = platformContext();
    Color oldFillColor = fillColor();

    if (oldFillColor != color)
        setCGFillColor(context.get(), color, colorSpace());

    bool drawOwnShadow = canUseShadowBlur();
    CGContextStateSaver stateSaver(context.get(), drawOwnShadow);
    if (drawOwnShadow) {
        clearCGDropShadow();

        const auto shadow = dropShadow();
        ASSERT(shadow);

        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawRectShadow(*this, FloatRoundedRect(rect));
    }

    CGContextFillRect(context.get(), rect);

    if (drawOwnShadow)
        stateSaver.restore();

    if (oldFillColor != color)
        setCGFillColor(context.get(), oldFillColor, colorSpace());
}

void GraphicsContextCG::fillRoundedRectImpl(const FloatRoundedRect& rect, const Color& color)
{
    RetainPtr context = platformContext();
    Color oldFillColor = fillColor();

    if (oldFillColor != color)
        setCGFillColor(context.get(), color, colorSpace());

    bool drawOwnShadow = canUseShadowBlur();
    CGContextStateSaver stateSaver(context.get(), drawOwnShadow);
    if (drawOwnShadow) {
        clearCGDropShadow();

        const auto shadow = dropShadow();
        ASSERT(shadow);

        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawRectShadow(*this, rect);
    }

    const FloatRect& r = rect.rect();
    const FloatRoundedRect::Radii& radii = rect.radii();
    bool equalWidths = (radii.topLeft().width() == radii.topRight().width() && radii.topRight().width() == radii.bottomLeft().width() && radii.bottomLeft().width() == radii.bottomRight().width());
    bool equalHeights = (radii.topLeft().height() == radii.bottomLeft().height() && radii.bottomLeft().height() == radii.topRight().height() && radii.topRight().height() == radii.bottomRight().height());
    bool hasCustomFill = fillGradient() || fillPattern();
    if (!hasCustomFill && equalWidths && equalHeights && radii.topLeft().width() * 2 == r.width() && radii.topLeft().height() * 2 == r.height())
        CGContextFillEllipseInRect(context.get(), r);
    else {
        Path path;
        path.addRoundedRect(rect);
        fillPath(path);
    }

    if (drawOwnShadow)
        stateSaver.restore();

    if (oldFillColor != color)
        setCGFillColor(context.get(), oldFillColor, colorSpace());
}

void GraphicsContextCG::fillRectWithRoundedHole(const FloatRect& rect, const FloatRoundedRect& roundedHoleRect, const Color& color)
{
    RetainPtr context = platformContext();

    Path path;
    path.addRect(rect);

    if (!roundedHoleRect.radii().isZero())
        path.addRoundedRect(roundedHoleRect);
    else
        path.addRect(roundedHoleRect.rect());

    WindRule oldFillRule = fillRule();
    Color oldFillColor = fillColor();

    setFillRule(WindRule::EvenOdd);
    setFillColor(color);

    // fillRectWithRoundedHole() assumes that the edges of rect are clipped out, so we only care about shadows cast around inside the hole.
    bool drawOwnShadow = canUseShadowBlur();
    CGContextStateSaver stateSaver(context.get(), drawOwnShadow);
    if (drawOwnShadow) {
        clearCGDropShadow();

        const auto shadow = dropShadow();
        ASSERT(shadow);

        ShadowBlur contextShadow(*shadow, shadowsIgnoreTransforms());
        contextShadow.drawInsetShadow(*this, rect, roundedHoleRect);
    }

    fillPath(path);

    if (drawOwnShadow)
        stateSaver.restore();

    setFillRule(oldFillRule);
    setFillColor(oldFillColor);
}

void GraphicsContextCG::resetClip()
{
    CGContextResetClip(platformContext());
}

void GraphicsContextCG::clip(const FloatRect& rect)
{
    CGContextClipToRect(platformContext(), rect);
}

void GraphicsContextCG::clipOut(const FloatRect& rect)
{
    // FIXME: Using CGRectInfinite is much faster than getting the clip bounding box. However, due
    // to <rdar://problem/12584492>, CGRectInfinite can't be used with an accelerated context that
    // has certain transforms that aren't just a translation or a scale. And due to <rdar://problem/14634453>
    // we cannot use it in for a printing context either.
    RetainPtr context = platformContext();
    const AffineTransform& ctm = getCTM();
    bool canUseCGRectInfinite = CGContextGetType(context.get()) != kCGContextTypePDF && (renderingMode() == RenderingMode::Unaccelerated || (!ctm.b() && !ctm.c()));
    CGRect rects[2] = { canUseCGRectInfinite ? CGRectInfinite : CGContextGetClipBoundingBox(context.get()), rect };
    CGContextBeginPath(context.get());
    CGContextAddRects(context.get(), rects, 2);
    CGContextEOClip(context.get());
}

void GraphicsContextCG::clipOut(const Path& path)
{
    RetainPtr context = platformContext();
    CGContextBeginPath(context.get());
    CGContextAddRect(context.get(), CGContextGetClipBoundingBox(context.get()));
    if (!path.isEmpty())
        addToCGContextPath(context.get(), path);
    CGContextEOClip(context.get());
}

void GraphicsContextCG::clipPath(const Path& path, WindRule clipRule)
{
    RetainPtr context = platformContext();
    if (path.isEmpty())
        CGContextClipToRect(context.get(), CGRectZero);
    else {
        setCGContextPath(context.get(), path);
        if (clipRule == WindRule::EvenOdd)
            CGContextEOClip(context.get());
        else
            CGContextClip(context.get());
    }
}

void GraphicsContextCG::clipToImageBuffer(ImageBuffer& imageBuffer, const FloatRect& destRect)
{
    auto nativeImage = imageBuffer.createNativeImageReference();
    if (!nativeImage)
        return;

    // FIXME: This image needs to be grayscale to be used as an alpha mask here.
    RetainPtr context = platformContext();
    CGContextTranslateCTM(context.get(), destRect.x(), destRect.maxY());
    CGContextScaleCTM(context.get(), 1, -1);
    CGContextClipToRect(context.get(), { { }, destRect.size() });
    CGContextClipToMask(context.get(), { { }, destRect.size() }, nativeImage->platformImage().get());
    CGContextScaleCTM(context.get(), 1, -1);
    CGContextTranslateCTM(context.get(), -destRect.x(), -destRect.maxY());
}

IntRect GraphicsContextCG::clipBounds() const
{
    return enclosingIntRect(CGContextGetClipBoundingBox(platformContext()));
}

void GraphicsContextCG::beginTransparencyLayer(float opacity)
{
    GraphicsContext::beginTransparencyLayer(opacity);

    save(GraphicsContextState::Purpose::TransparencyLayer);

    RetainPtr context = platformContext();
    CGContextSetAlpha(context.get(), opacity);
    CGContextBeginTransparencyLayer(context.get(), 0);

    m_userToDeviceTransformKnownToBeIdentity = false;
}

void GraphicsContextCG::beginTransparencyLayer(CompositeOperator, BlendMode)
{
    // Passing state().alpha() to beginTransparencyLayer(opacity) will
    // preserve the current global alpha.
    beginTransparencyLayer(state().alpha());
}

void GraphicsContextCG::endTransparencyLayer()
{
    GraphicsContext::endTransparencyLayer();

    RetainPtr context = platformContext();
    CGContextEndTransparencyLayer(context.get());

    restore(GraphicsContextState::Purpose::TransparencyLayer);
}

static CGFloat scaledBlurRadius(CGFloat blurRadius, const CGAffineTransform& userToBaseCTM, bool shadowsIgnoreTransforms)
{
    if (!shadowsIgnoreTransforms) {
        CGFloat A = userToBaseCTM.a * userToBaseCTM.a + userToBaseCTM.b * userToBaseCTM.b;
        CGFloat B = userToBaseCTM.a * userToBaseCTM.c + userToBaseCTM.b * userToBaseCTM.d;
        CGFloat C = B;
        CGFloat D = userToBaseCTM.c * userToBaseCTM.c + userToBaseCTM.d * userToBaseCTM.d;

        CGFloat smallEigenvalue = narrowPrecisionToCGFloat(sqrt(0.5 * ((A + D) - sqrt(4 * B * C + (A - D) * (A - D)))));

        blurRadius *= smallEigenvalue;
    }

    // Extreme "blur" values can make text drawing crash or take crazy long times, so clamp
    return std::min(blurRadius, narrowPrecisionToCGFloat(1000.0));
}

void GraphicsContextCG::setCGDropShadow(const std::optional<GraphicsDropShadow>& shadow, bool shadowsIgnoreTransforms)
{
    if (!shadow || !shadow->color.isValid() || (shadow->offset.isZero() && !shadow->radius)) {
        clearCGDropShadow();
        return;
    }

    RetainPtr context = platformContext();
    CGAffineTransform userToBaseCTM = getUserToBaseCTM(context.get());
    CGFloat blurRadius = scaledBlurRadius(shadow->radius, userToBaseCTM, shadowsIgnoreTransforms);

    CGSize offset = shadow->offset;
    if (!shadowsIgnoreTransforms)
        offset = CGSizeApplyAffineTransform(offset, userToBaseCTM);

    CGContextSetAlpha(context.get(), shadow->opacity);

    auto style = adoptCF(CGStyleCreateShadow2(offset, blurRadius, cachedSDRCGColorForColorspace(shadow->color, colorSpace()).get()));
    CGContextSetStyle(context.get(), style.get());
}

void GraphicsContextCG::clearCGDropShadow()
{
    CGContextSetStyle(platformContext(), nullptr);
}

#if HAVE(CGSTYLE_COLORMATRIX_BLUR)
void GraphicsContextCG::setCGGaussianBlur(const GraphicsGaussianBlur& gaussianBlur, bool shadowsIgnoreTransforms)
{
    RetainPtr context = platformContext();

    ASSERT(gaussianBlur.radius.width() == gaussianBlur.radius.height());

    CGAffineTransform userToBaseCTM = getUserToBaseCTM(context.get());
    CGFloat blurRadius = scaledBlurRadius(gaussianBlur.radius.width(), userToBaseCTM, shadowsIgnoreTransforms);

    CGGaussianBlurStyle gaussianBlurStyle = { 1, blurRadius };
    auto style = adoptCF(CGStyleCreateGaussianBlur(&gaussianBlurStyle));
    CGContextSetStyle(context.get(), style.get());
}

void GraphicsContextCG::setCGColorMatrix(const GraphicsColorMatrix& colorMatrix)
{
    RetainPtr context = platformContext();

    CGColorMatrixStyle cgColorMatrix = { 1, { 0 } };
    for (auto [dst, src] : zippedRange(cgColorMatrix.matrix, colorMatrix.values))
        dst = src;
    auto style = adoptCF(CGStyleCreateColorMatrix(&cgColorMatrix));
    CGContextSetStyle(context.get(), style.get());
}
#endif

void GraphicsContextCG::setCGStyle(const std::optional<GraphicsStyle>& style, bool shadowsIgnoreTransforms)
{
    RetainPtr context = platformContext();

    if (!style) {
        CGContextSetStyle(context.get(), nullptr);
        return;
    }

    WTF::switchOn(*style,
        [&] (const GraphicsDropShadow& dropShadow) {
            setCGDropShadow(dropShadow, shadowsIgnoreTransforms);
        },
        [&] (const GraphicsGaussianBlur& gaussianBlur) {
#if HAVE(CGSTYLE_COLORMATRIX_BLUR)
            setCGGaussianBlur(gaussianBlur, shadowsIgnoreTransforms);
#else
            ASSERT_NOT_REACHED();
            UNUSED_PARAM(gaussianBlur);
#endif
        },
        [&] (const GraphicsColorMatrix& colorMatrix) {
#if HAVE(CGSTYLE_COLORMATRIX_BLUR)
            setCGColorMatrix(colorMatrix);
#else
            ASSERT_NOT_REACHED();
            UNUSED_PARAM(colorMatrix);
#endif
        }
    );
}

void GraphicsContextCG::didUpdateState(GraphicsContextState& state)
{
    if (!state.changes())
        return;

    RetainPtr context = platformContext();

    for (auto change : state.changes()) {
        switch (change) {
        case GraphicsContextState::Change::FillBrush:
            setCGFillColor(context.get(), state.fillBrush().color(), colorSpace());
            break;

        case GraphicsContextState::Change::StrokeThickness:
            CGContextSetLineWidth(context.get(), std::max(state.strokeThickness(), 0.f));
            break;

        case GraphicsContextState::Change::StrokeBrush:
            CGContextSetStrokeColorWithColor(context.get(), cachedSDRCGColorForColorspace(state.strokeBrush().color(), colorSpace()).get());
            break;

        case GraphicsContextState::Change::CompositeMode:
            setCGBlendMode(context.get(), state.compositeMode().operation, state.compositeMode().blendMode);
            break;

        case GraphicsContextState::Change::DropShadow:
            setCGDropShadow(state.dropShadow(), state.shadowsIgnoreTransforms());
            break;

        case GraphicsContextState::Change::Style:
            setCGStyle(state.style(), state.shadowsIgnoreTransforms());
            break;

        case GraphicsContextState::Change::Alpha:
            CGContextSetAlpha(context.get(), state.alpha());
            break;

        case GraphicsContextState::Change::ImageInterpolationQuality:
            CGContextSetInterpolationQuality(context.get(), toCGInterpolationQuality(state.imageInterpolationQuality()));
            break;

        case GraphicsContextState::Change::TextDrawingMode:
            CGContextSetTextDrawingMode(context.get(), cgTextDrawingMode(state.textDrawingMode()));
            break;

        case GraphicsContextState::Change::ShouldAntialias:
            CGContextSetShouldAntialias(context.get(), state.shouldAntialias());
            break;

        case GraphicsContextState::Change::ShouldSmoothFonts:
            CGContextSetShouldSmoothFonts(context.get(), state.shouldSmoothFonts());
            break;

        default:
            break;
        }
    }

    state.didApplyChanges();
}

void GraphicsContextCG::setMiterLimit(float limit)
{
    CGContextSetMiterLimit(platformContext(), limit);
}

void GraphicsContextCG::clearRect(const FloatRect& r)
{
    CGContextClearRect(platformContext(), r);
}

void GraphicsContextCG::strokeRect(const FloatRect& rect, float lineWidth)
{
    RetainPtr context = platformContext();

    if (RefPtr strokeGradient = this->strokeGradient()) {
        if (hasDropShadow()) {
            const float doubleLineWidth = lineWidth * 2;
            float adjustedWidth = ceilf(rect.width() + doubleLineWidth);
            float adjustedHeight = ceilf(rect.height() + doubleLineWidth);
            FloatSize layerSize = getCTM().mapSize(FloatSize(adjustedWidth, adjustedHeight));

            auto layer = adoptCF(CGLayerCreateWithContext(context.get(), layerSize, 0));

            RetainPtr layerContext = CGLayerGetContext(layer.get());
            CGContextSetLineWidth(layerContext.get(), lineWidth);

            // Compensate for the line width, otherwise the layer's top-left corner would be
            // aligned with the rect's top-left corner. This would result in leaving pixels out of
            // the layer on the left and top sides.
            const float translationX = lineWidth - rect.x();
            const float translationY = lineWidth - rect.y();
            CGContextScaleCTM(layerContext.get(), layerSize.width() / adjustedWidth, layerSize.height() / adjustedHeight);
            CGContextTranslateCTM(layerContext.get(), translationX, translationY);

            CGContextAddRect(layerContext.get(), rect);
            CGContextReplacePathWithStrokedPath(layerContext.get());
            CGContextClip(layerContext.get());
            CGContextConcatCTM(layerContext.get(), strokeGradientSpaceTransform());
            strokeGradient->paint(layerContext.get());

            const float destinationX = roundf(rect.x() - lineWidth);
            const float destinationY = roundf(rect.y() - lineWidth);
            CGContextDrawLayerInRect(context.get(), CGRectMake(destinationX, destinationY, adjustedWidth, adjustedHeight), layer.get());
        } else {
            CGContextStateSaver stateSaver(context.get());
            setStrokeThickness(lineWidth);
            CGContextAddRect(context.get(), rect);
            CGContextReplacePathWithStrokedPath(context.get());
            CGContextClip(context.get());
            CGContextConcatCTM(context.get(), strokeGradientSpaceTransform());
            strokeGradient->paint(*this);
        }
        return;
    }

    if (strokePattern())
        applyStrokePattern();

    // Using CGContextAddRect and CGContextStrokePath to stroke rect rather than
    // convenience functions (CGContextStrokeRect/CGContextStrokeRectWithWidth).
    // The convenience functions currently (in at least OSX 10.9.4) fail to
    // apply some attributes of the graphics state in certain cases
    // (as identified in https://bugs.webkit.org/show_bug.cgi?id=132948)
    CGContextStateSaver stateSaver(context.get());
    setStrokeThickness(lineWidth);

    CGContextAddRect(context.get(), rect);
    CGContextStrokePath(context.get());
}

void GraphicsContextCG::setLineCap(LineCap cap)
{
    switch (cap) {
    case LineCap::Butt:
        CGContextSetLineCap(platformContext(), kCGLineCapButt);
        break;
    case LineCap::Round:
        CGContextSetLineCap(platformContext(), kCGLineCapRound);
        break;
    case LineCap::Square:
        CGContextSetLineCap(platformContext(), kCGLineCapSquare);
        break;
    }
}

void GraphicsContextCG::setLineDash(const DashArray& dashes, float dashOffset)
{
    if (dashOffset < 0) {
        float length = 0;
        for (size_t i = 0; i < dashes.size(); ++i)
            length += static_cast<float>(dashes[i]);
        if (length)
            dashOffset = fmod(dashOffset, length) + length;
    }
    auto dashesSpan = dashes.span();
    CGContextSetLineDash(platformContext(), dashOffset, dashesSpan.data(), dashesSpan.size());
}

void GraphicsContextCG::setLineJoin(LineJoin join)
{
    switch (join) {
    case LineJoin::Miter:
        CGContextSetLineJoin(platformContext(), kCGLineJoinMiter);
        break;
    case LineJoin::Round:
        CGContextSetLineJoin(platformContext(), kCGLineJoinRound);
        break;
    case LineJoin::Bevel:
        CGContextSetLineJoin(platformContext(), kCGLineJoinBevel);
        break;
    }
}

void GraphicsContextCG::scale(const FloatSize& size)
{
    CGContextScaleCTM(platformContext(), size.width(), size.height());
    m_userToDeviceTransformKnownToBeIdentity = false;
}

void GraphicsContextCG::rotate(float angle)
{
    CGContextRotateCTM(platformContext(), angle);
    m_userToDeviceTransformKnownToBeIdentity = false;
}

void GraphicsContextCG::translate(float x, float y)
{
    CGContextTranslateCTM(platformContext(), x, y);
    m_userToDeviceTransformKnownToBeIdentity = false;
}

void GraphicsContextCG::concatCTM(const AffineTransform& transform)
{
    CGContextConcatCTM(platformContext(), transform);
    m_userToDeviceTransformKnownToBeIdentity = false;
}

void GraphicsContextCG::setCTM(const AffineTransform& transform)
{
    CGContextSetCTM(platformContext(), transform);
    m_userToDeviceTransformKnownToBeIdentity = false;
}

AffineTransform GraphicsContextCG::getCTM(IncludeDeviceScale includeScale) const
{
    // The CTM usually includes the deviceScaleFactor except in WebKit 1 when the
    // content is non-composited, since the scale factor is integrated at a lower
    // level. To guarantee the deviceScale is included, we can use this CG API.
    if (includeScale == DefinitelyIncludeDeviceScale)
        return CGContextGetUserSpaceToDeviceSpaceTransform(platformContext());

    return CGContextGetCTM(platformContext());
}

FloatRect GraphicsContextCG::roundToDevicePixels(const FloatRect& rect) const
{
    CGAffineTransform deviceMatrix;
    if (!m_userToDeviceTransformKnownToBeIdentity) {
        deviceMatrix = CGContextGetUserSpaceToDeviceSpaceTransform(contextForState());
        if (CGAffineTransformIsIdentity(deviceMatrix))
            m_userToDeviceTransformKnownToBeIdentity = true;
    }
    if (m_userToDeviceTransformKnownToBeIdentity)
        return roundedIntRect(rect);
    return cgRoundToDevicePixelsNonIdentity(deviceMatrix, rect);
}

void GraphicsContextCG::drawLinesForText(const FloatPoint& origin, float thickness, std::span<const FloatSegment> lineSegments, bool isPrinting, bool doubleLines, StrokeStyle strokeStyle)
{
    auto [rects, color] = computeRectsAndStrokeColorForLinesForText(origin, thickness, lineSegments, isPrinting, doubleLines, strokeStyle);
    if (rects.isEmpty())
        return;
    bool changeFillColor = fillColor() != color;
    if (changeFillColor)
        setCGFillColor(platformContext(), color, colorSpace());
    CGContextFillRects(platformContext(), rects.span().data(), rects.size());
    if (changeFillColor)
        setCGFillColor(platformContext(), fillColor(), colorSpace());
}

void GraphicsContextCG::setURLForRect(const URL& link, const FloatRect& destRect)
{
    RetainPtr<CFURLRef> urlRef = link.createCFURL();
    if (!urlRef)
        return;

    RetainPtr context = platformContext();

    FloatRect rect = destRect;
    // Get the bounding box to handle clipping.
    rect.intersect(CGContextGetClipBoundingBox(context.get()));

    CGPDFContextSetURLForRect(context.get(), urlRef.get(), CGRectApplyAffineTransform(rect, CGContextGetCTM(context.get())));
}

bool GraphicsContextCG::isCALayerContext() const
{
    return m_isLayerCGContext;
}

bool GraphicsContextCG::knownToHaveFloatBasedBacking() const
{
    RetainPtr context = platformContext();

    if (CGContextGetType(context.get()) == kCGContextTypeIOSurface)
        return CGIOSurfaceContextGetBitmapInfo(context.get()) & kCGBitmapFloatComponents;
    if (CGContextGetType(context.get()) == kCGContextTypeBitmap)
        return CGBitmapContextGetBitmapInfo(context.get()) & kCGBitmapFloatComponents;
    return false;
}

RenderingMode GraphicsContextCG::renderingMode() const
{
    return m_renderingMode;
}

void GraphicsContextCG::applyDeviceScaleFactor(float deviceScaleFactor)
{
    GraphicsContext::applyDeviceScaleFactor(deviceScaleFactor);

    // CoreGraphics expects the base CTM of a HiDPI context to have the scale factor applied to it.
    // Failing to change the base level CTM will cause certain CG features, such as focus rings,
    // to draw with a scale factor of 1 rather than the actual scale factor.
    CGContextSetBaseCTM(platformContext(), CGAffineTransformScale(CGContextGetBaseCTM(platformContext()), deviceScaleFactor, deviceScaleFactor));
}

void GraphicsContextCG::fillEllipse(const FloatRect& ellipse)
{
    // CGContextFillEllipseInRect only supports solid colors.
    if (fillGradient() || fillPattern()) {
        fillEllipseAsPath(ellipse);
        return;
    }

    RetainPtr context = platformContext();
    CGContextFillEllipseInRect(context.get(), ellipse);
}

void GraphicsContextCG::strokeEllipse(const FloatRect& ellipse)
{
    // CGContextStrokeEllipseInRect only supports solid colors.
    if (strokeGradient() || strokePattern()) {
        strokeEllipseAsPath(ellipse);
        return;
    }

    RetainPtr context = platformContext();
    CGContextStrokeEllipseInRect(context.get(), ellipse);
}

void GraphicsContextCG::beginPage(const FloatRect& pageRect)
{
    RetainPtr context = platformContext();

    if (CGContextGetType(context.get()) != kCGContextTypePDF) {
        ASSERT_NOT_REACHED();
        return;
    }

    auto mediaBox = CGRectMake(pageRect.x(), pageRect.y(), pageRect.width(), pageRect.height());
    auto mediaBoxData = adoptCF(CFDataCreate(nullptr, (const UInt8 *)&mediaBox, sizeof(CGRect)));

    const void* key = kCGPDFContextMediaBox;
    const void* value = mediaBoxData.get();
    auto pageInfo = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CGPDFContextBeginPage(context.get(), pageInfo.get());
}

void GraphicsContextCG::endPage()
{
    RetainPtr context = platformContext();

    if (CGContextGetType(context.get()) != kCGContextTypePDF) {
        ASSERT_NOT_REACHED();
        return;
    }

    CGPDFContextEndPage(context.get());
}

bool GraphicsContextCG::supportsInternalLinks() const
{
    return true;
}

void GraphicsContextCG::setDestinationForRect(const String& name, const FloatRect& destRect)
{
    RetainPtr context = platformContext();

    FloatRect rect = destRect;
    rect.intersect(CGContextGetClipBoundingBox(context.get()));

    CGRect transformedRect = CGRectApplyAffineTransform(rect, CGContextGetCTM(context.get()));
    CGPDFContextSetDestinationForRect(context.get(), name.createCFString().get(), transformedRect);
}

void GraphicsContextCG::addDestinationAtPoint(const String& name, const FloatPoint& position)
{
    RetainPtr context = platformContext();
    CGPoint transformedPoint = CGPointApplyAffineTransform(position, CGContextGetCTM(context.get()));
    CGPDFContextAddDestinationAtPoint(context.get(), name.createCFString().get(), transformedPoint);
}

bool GraphicsContextCG::canUseShadowBlur() const
{
    return (renderingMode() == RenderingMode::Unaccelerated) && hasBlurredDropShadow() && !m_state.shadowsIgnoreTransforms();
}

bool GraphicsContextCG::consumeHasDrawn()
{
    bool hasDrawn = m_hasDrawn;
    m_hasDrawn = false;
    return hasDrawn;
}

#if HAVE(SUPPORT_HDR_DISPLAY)
void GraphicsContextCG::setMaxEDRHeadroom(std::optional<float> headroom)
{
    m_maxEDRHeadroom = headroom;
}
#endif


}

#endif
