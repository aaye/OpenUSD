//
// Copyright 2020 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "pxr/imaging/hdx/aovInputTask.h"
#include "pxr/imaging/hdx/hgiConversions.h"

#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hdSt/renderBuffer.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgi/blitCmds.h"
#include "pxr/imaging/hgi/blitCmdsOps.h"


PXR_NAMESPACE_OPEN_SCOPE

HdxAovInputTask::HdxAovInputTask(HdSceneDelegate* delegate, SdfPath const& id)
 : HdxTask(id)
 , _converged(false)
 , _aovBufferPath()
 , _depthBufferPath()
 , _aovBuffer(nullptr)
 , _depthBuffer(nullptr)
{
}

HdxAovInputTask::~HdxAovInputTask()
{
    if (_aovTexture) {
        _GetHgi()->DestroyTexture(&_aovTexture);
    }
    if (_aovTextureIntermediate) {
        _GetHgi()->DestroyTexture(&_aovTextureIntermediate);
    }
    if (_depthTexture) {
        _GetHgi()->DestroyTexture(&_depthTexture);
    }
    if (_depthTextureIntermediate) {
        _GetHgi()->DestroyTexture(&_depthTextureIntermediate);
    }
}

bool
HdxAovInputTask::IsConverged() const
{
    return _converged;
}

void
HdxAovInputTask::_Sync(HdSceneDelegate* delegate,
                      HdTaskContext* ctx,
                      HdDirtyBits* dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    if ((*dirtyBits) & HdChangeTracker::DirtyParams) {
        HdxAovInputTaskParams params;

        if (_GetTaskParams(delegate, &params)) {
            _aovBufferPath = params.aovBufferPath;
            _depthBufferPath = params.depthBufferPath;
        }
    }
    *dirtyBits = HdChangeTracker::Clean;
}

void
HdxAovInputTask::Prepare(HdTaskContext* ctx, HdRenderIndex *renderIndex)
{
    // Wrap one HdEngine::Execute frame with Hgi StartFrame and EndFrame.
    // EndFrame is currently called in the PresentTask.
    // This is important for Hgi garbage collection to run.
    _GetHgi()->StartFrame();

    _aovBuffer = nullptr;
    _depthBuffer = nullptr;

    // An empty _aovBufferPath disables the task
    if (!_aovBufferPath.IsEmpty()) {
        _aovBuffer = static_cast<HdRenderBuffer*>(
            renderIndex->GetBprim(
                HdPrimTypeTokens->renderBuffer, _aovBufferPath));
    }

    if (!_depthBufferPath.IsEmpty()) {
        _depthBuffer = static_cast<HdRenderBuffer*>(
            renderIndex->GetBprim(
                HdPrimTypeTokens->renderBuffer, _depthBufferPath));
    }

    // Create / update the texture that will be used to ping-pong between color
    // targets in tasks that wish to read from and write to the color target.
    if (_aovBuffer) {
        _UpdateIntermediateTexture(_aovTextureIntermediate, _aovBuffer,
                                   HgiTextureUsageBitsColorTarget);
    }

    // Do the same for the intermediate depth texture.
    if (_depthBuffer) {
        _UpdateIntermediateTexture(_depthTextureIntermediate, _depthBuffer,
                                   HgiTextureUsageBitsDepthTarget);
    }
}

void
HdxAovInputTask::Execute(HdTaskContext* ctx)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // This task requires an aov buffer to have been set and is immediately
    // converged if there is no aov buffer.
    if (!_aovBuffer) {
        _converged = true;
        return;
    }

    // Check converged state of buffer(s)
    _converged = _aovBuffer->IsConverged();
    if (_depthBuffer) {
        _converged = _converged && _depthBuffer->IsConverged();
    }

    // Resolve the buffers before we read them.
    _aovBuffer->Resolve();
    if (_depthBuffer) {
        _depthBuffer->Resolve();
    }

    // Start by clearing aov texture handles from task context.
    // These are last frames textures and we may be visualizing different aovs.
    ctx->erase(HdAovTokens->color);
    ctx->erase(HdAovTokens->depth);
    ctx->erase(HdxAovTokens->colorIntermediate);
    ctx->erase(HdxAovTokens->depthIntermediate);

    // If the aov is already backed by a HgiTexture we skip creating a new
    // GPU HgiTexture for it and place it directly on the shared task context
    // for consecutive tasks to find and operate on.
    // The lifetime management of that HgiTexture remains with the aov.

    bool hgiHandleProvidedByAov = false;
    const bool mulSmp = false;

    VtValue aov = _aovBuffer->GetResource(mulSmp);
    if (aov.IsHolding<HgiTextureHandle>()) {
        hgiHandleProvidedByAov = true;
        (*ctx)[HdAovTokens->color] = aov;
    }

    (*ctx)[HdxAovTokens->colorIntermediate] = VtValue(_aovTextureIntermediate);

    if (_depthBuffer) {
        VtValue depth = _depthBuffer->GetResource(mulSmp);
        if (depth.IsHolding<HgiTextureHandle>()) {
            (*ctx)[HdAovTokens->depth] = depth;
        }

        (*ctx)[HdxAovTokens->depthIntermediate] =
            VtValue(_depthTextureIntermediate);
    }

    if (hgiHandleProvidedByAov) {
        return;
    }

    // If the aov is not backed by a HgiTexture (e.g. RenderMan, Embree) we
    // convert the aov pixel data to a HgiTexture and place that new texture
    // in the shared task context.
    // The lifetime of this new HgiTexture is managed by this task. 

    _UpdateTexture(ctx, _aovTexture, _aovBuffer,
        HgiTextureUsageBitsColorTarget);
    if (_aovTexture) {
        (*ctx)[HdAovTokens->color] = VtValue(_aovTexture);
    }

    if (_depthBuffer) {
        _UpdateTexture(ctx, _depthTexture, _depthBuffer,
            HgiTextureUsageBitsDepthTarget);
        if (_depthTexture) {
            (*ctx)[HdAovTokens->depth] = VtValue(_depthTexture);
        }
    }
}

namespace {
void
_ConvertRGBtoRGBA(const float* rgbValues,
                  size_t numRgbValues,
                  std::vector<float>* rgbaValues)
{
    if (numRgbValues % 3 != 0) {
        TF_WARN("Value count should be divisible by 3.");
        return;
    }

    const size_t numRgbaValues = numRgbValues * 4 / 3;

    if (rgbValues != nullptr && rgbaValues != nullptr) {
        const float *rgbValuesIt = rgbValues;
        rgbaValues->resize(numRgbaValues);
        float *rgbaValuesIt = rgbaValues->data();
        const float * const end = rgbaValuesIt + numRgbaValues;

        while (rgbaValuesIt != end) {
            *rgbaValuesIt++ = *rgbValuesIt++;
            *rgbaValuesIt++ = *rgbValuesIt++;
            *rgbaValuesIt++ = *rgbValuesIt++;
            *rgbaValuesIt++ = 1.0f;
        }
    }
}
} // anonymous namespace

void
HdxAovInputTask::_UpdateTexture(
    HdTaskContext* ctx,
    HgiTextureHandle& texture,
    HdRenderBuffer* buffer,
    HgiTextureUsageBits usage)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    const GfVec3i dim(
        buffer->GetWidth(),
        buffer->GetHeight(),
        buffer->GetDepth());

    const void* pixelData = buffer->Map();

    HdFormat hdFormat = buffer->GetFormat();
    // HgiFormatFloat32Vec3 not a supported texture format for Vulkan. Convert
    // data to vec4 format.
    if (hdFormat == HdFormatFloat32Vec3) {
        hdFormat = HdFormatFloat32Vec4;
        const size_t numValues = 3 * dim[0] * dim[1] * dim[2];
        std::vector<float> float4Data;
        _ConvertRGBtoRGBA(
            reinterpret_cast<const float*>(pixelData), numValues, &float4Data);
        pixelData = reinterpret_cast<const void*>(float4Data.data());
    }

    const HgiFormat bufFormat = HdxHgiConversions::GetHgiFormat(hdFormat);
    const size_t pixelByteSize = HdDataSizeOfFormat(hdFormat);
    const size_t dataByteSize = dim[0] * dim[1] * dim[2] * pixelByteSize;

    // Update the existing texture if specs are compatible. This is more
    // efficient than re-creating, because the underlying framebuffer that
    // had the old texture attached would also need to be re-created.
    if (texture && texture->GetDescriptor().dimensions == dim &&
            texture->GetDescriptor().format == bufFormat) {
        HgiTextureCpuToGpuOp copyOp;
        copyOp.bufferByteSize = dataByteSize;
        copyOp.cpuSourceBuffer = pixelData;
        copyOp.gpuDestinationTexture = texture;
        HgiBlitCmdsUniquePtr blitCmds = _GetHgi()->CreateBlitCmds();
        blitCmds->PushDebugGroup("Upload CPU texels");
        blitCmds->CopyTextureCpuToGpu(copyOp);
        blitCmds->PopDebugGroup();
        _GetHgi()->SubmitCmds(blitCmds.get());
    } else {
        // Destroy old texture
        if(texture) {
            _GetHgi()->DestroyTexture(&texture);
        }
        // Create a new texture
        HgiTextureDesc texDesc;
        texDesc.debugName = "AovInput Texture";
        texDesc.dimensions = dim;
        texDesc.format = bufFormat;
        texDesc.initialData = pixelData;
        texDesc.layerCount = 1;
        texDesc.mipLevels = 1;
        texDesc.pixelsByteSize = dataByteSize;
        texDesc.sampleCount = HgiSampleCount1;
        texDesc.usage = usage | HgiTextureUsageBitsShaderRead;

        texture = _GetHgi()->CreateTexture(texDesc);
    }
    buffer->Unmap();
}

void
HdxAovInputTask::_UpdateIntermediateTexture(
    HgiTextureHandle& texture,
    HdRenderBuffer* buffer,
    HgiTextureUsageBits usage)
{
    const GfVec3i dim(
        buffer->GetWidth(),
        buffer->GetHeight(),
        buffer->GetDepth());
    
    // HgiFormatFloat32Vec3 not a supported texture format for Vulkan. Use vec4 
    // format instead.
    HdFormat hdFormat = buffer->GetFormat();
    if (hdFormat == HdFormatFloat32Vec3) {
        hdFormat = HdFormatFloat32Vec4;
    }

    HgiFormat hgiFormat =
        HdxHgiConversions::GetHgiFormat(hdFormat);

    if (texture) {
        HgiTextureDesc const& desc =
            texture->GetDescriptor();
        if (dim != desc.dimensions || hgiFormat != desc.format) {
            _GetHgi()->DestroyTexture(&texture);
        }
    }

    if (!texture) {

        HgiTextureDesc texDesc;
        texDesc.debugName = "AovInput Intermediate Texture";
        texDesc.dimensions = dim;

        texDesc.format = hgiFormat;
        texDesc.layerCount = 1;
        texDesc.mipLevels = 1;
        texDesc.sampleCount = HgiSampleCount1;
        texDesc.usage = usage | HgiTextureUsageBitsShaderRead;

        texture = _GetHgi()->CreateTexture(texDesc);
    }
}

// --------------------------------------------------------------------------- //
// VtValue Requirements
// --------------------------------------------------------------------------- //

std::ostream& operator<<(std::ostream& out, const HdxAovInputTaskParams& pv)
{
    out << "AovInputTask Params: (...) "
        << pv.aovBufferPath << " "
        << pv.depthBufferPath;
    return out;
}

bool operator==(const HdxAovInputTaskParams& lhs,
                const HdxAovInputTaskParams& rhs)
{
    return lhs.aovBufferPath   == rhs.aovBufferPath    &&
           lhs.depthBufferPath == rhs.depthBufferPath;
}

bool operator!=(const HdxAovInputTaskParams& lhs,
                const HdxAovInputTaskParams& rhs)
{
    return !(lhs == rhs);
}

PXR_NAMESPACE_CLOSE_SCOPE
