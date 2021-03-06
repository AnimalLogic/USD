//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/glf/glew.h"

#include "pxr/imaging/cameraUtil/conformWindow.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hdx/drawTargetRenderPass.h"
#include "pxr/imaging/hdx/drawTargetTask.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hdx/debugCodes.h"
#include "pxr/imaging/hdSt/drawTarget.h"
#include "pxr/imaging/hdSt/renderPassState.h"
#include "pxr/imaging/hdSt/simpleLightingShader.h"
#include "pxr/imaging/hdSt/dynamicUvTextureObject.h"
#include "pxr/imaging/glf/drawTarget.h"
#include "pxr/imaging/glf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE

struct
HdxDrawTargetTask::_RenderPassInfo
{
    HdStRenderPassStateSharedPtr renderPassState;
    HdStSimpleLightingShaderSharedPtr simpleLightingShader;
    HdStDrawTarget *target;
    unsigned int version;
};

static
HdCompareFunction
HdxDrawTargetTask_GetResolvedDepthFunc(HdCompareFunction depthFunc,
                                       HdDepthPriority  priority)
{
    static const HdCompareFunction
        ResolvedDepthFunc[HdDepthPriorityCount][HdCmpFuncLast] =
    {
        // HdDepthPriorityNearest
        {
            HdCmpFuncNever,     // HdCmpFuncNever
            HdCmpFuncLess,      // HdCmpFuncLess
            HdCmpFuncEqual,     // HdCmpFuncEqual
            HdCmpFuncLEqual,    // HdCmpFuncLEqual
            HdCmpFuncGreater,   // HdCmpFuncGreater
            HdCmpFuncNotEqual,  // HdCmpFuncNotEqual
            HdCmpFuncGEqual,    // HdCmpFuncGEqual
            HdCmpFuncAlways,    // HdCmpFuncAlways
        },

        // HdDepthPriorityFarthest
        {
            HdCmpFuncNever,     // HdCmpFuncNever
            HdCmpFuncGEqual,    // HdCmpFuncLess
            HdCmpFuncEqual,     // HdCmpFuncEqual
            HdCmpFuncGreater,   // HdCmpFuncLEqual
            HdCmpFuncLEqual,    // HdCmpFuncGreater
            HdCmpFuncNotEqual,  // HdCmpFuncNotEqual
            HdCmpFuncLess,      // HdCmpFuncGEqual
            HdCmpFuncAlways,    // HdCmpFuncAlways
        },
    };

    return ResolvedDepthFunc[priority][depthFunc];
}

HdxDrawTargetTask::HdxDrawTargetTask(HdSceneDelegate* delegate,
                                     SdfPath const& id)
 : HdTask(id)
 , _currentDrawTargetSetVersion(0)
 , _renderPassesInfo()
 , _renderPasses()
 , _overrideColor()
 , _wireframeColor()
 , _enableLighting(false)
 , _alphaThreshold(0.0f)
 , _depthBiasUseDefault(true)
 , _depthBiasEnable(false)
 , _depthBiasConstantFactor(0.0f)
 , _depthBiasSlopeFactor(1.0f)
 , _depthFunc(HdCmpFuncLEqual)
 , _cullStyle(HdCullStyleBackUnlessDoubleSided)
 , _enableSampleAlphaToCoverage(true)
 , _renderTags()
{
}

HdxDrawTargetTask::~HdxDrawTargetTask() = default;

namespace {

//
// Topological sorting of the draw targets based on their
// inter-dependencies.
//

bool
_DoesCollectionContainPath(HdRprimCollection const &collection,
                           SdfPath const& path)
{
    for (SdfPath const &excludePath : collection.GetExcludePaths()) {
        if (path.HasPrefix(excludePath)) {
            return false;
        }
    }
    for (SdfPath const &rootPath : collection.GetRootPaths()) {
        if (path.HasPrefix(rootPath)) {
            return true;
        }
    }
    return false;
}

// Determines whether the collection of the first draw target contains
// the path of the second draw target.
//
// This is used as a simple heuristic to determine the dependencies
// between draw targets. In theory, one could imagine a scenaria where
// this is not correct: a draw target collection includes a piece of
// geometry but not the draw target that serves as texture for the
// geometry. See HYD-1833.
//
// Once we have better tracking of the prim depedencies in hydra,
// we can address this in a better way.
bool
_IsDependentOn(HdStDrawTarget const *drawTarget,
               HdStDrawTarget const *otherDrawTarget)
{
    return
        drawTarget &&
        otherDrawTarget &&
        drawTarget != otherDrawTarget &&
        _DoesCollectionContainPath(drawTarget->GetCollection(),
                                   otherDrawTarget->GetId());
}

// Information returned by topological sort
struct _DrawTargetEntry
{
    // Index in draw target vector created by namespace traversal
    size_t originalIndex;
    // The draw target
    HdStDrawTarget * drawTarget;
    // Do other draw targets depend on this one?
    bool hasDependentDrawTargets;
};

using _DrawTargetEntryVector = std::vector<_DrawTargetEntry>;

// Topologically sort draw targets.
static
void
_SortDrawTargets(HdStDrawTargetPtrVector const &drawTargets,
                 _DrawTargetEntryVector * result)
{
    TRACE_FUNCTION();

    if (drawTargets.empty()) {
        return;
    }

    // Number of draw targets
    const size_t n = drawTargets.size();

    // Index of draw target to indices of draw targets it depends on
    std::vector<std::set<size_t>>    indexToDependencies(n);
    // Index of draw target to indices of draw targets that depend on it
    std::vector<std::vector<size_t>> indexToDependents(n);

    {
        TRACE_FUNCTION_SCOPE("Computing drawtarget dependencies");

        // Determine which draw target depends on which
        for (size_t dependent = 0; dependent < n; dependent++) {
            for (size_t dependency = 0; dependency < n; dependency++) {
                if (_IsDependentOn(drawTargets[dependent],
                                   drawTargets[dependency])) {
                    indexToDependencies[dependent].insert(dependency);
                    indexToDependents[dependency].push_back(dependent);
                }
            }
        }
    }

    {
        TRACE_FUNCTION_SCOPE("Topological sort");

        // Start by scheduling draw targets that do not depend on
        // any other draw target.
        result->reserve(n);
        for (size_t dependent = 0; dependent < n; dependent++) {
            if (indexToDependencies[dependent].empty()) {
                result->push_back(
                    {dependent, drawTargets[dependent], false});
            }
        }

        // Iterate through all scheduled draw targets (while scheduling
        // new draw targets).
        for (size_t i = 0; i < result->size(); i++) {
            _DrawTargetEntry &entry = (*result)[i];
            const size_t dependency = entry.originalIndex;
            // For each draw target that depends on this draw target.
            for (const size_t dependent : indexToDependents[dependency]) {
                // Since this draw target has been scheduled, remove it as
                // dependency.
                indexToDependencies[dependent].erase(dependency);
                // If this was the last dependency of the other draw
                // target, we can schedule the other draw target.
                if (indexToDependencies[dependent].empty()) {
                    result->push_back(
                        {dependent, drawTargets[dependent], false});
                }
                entry.hasDependentDrawTargets = true;
            }
        }
        
        // Infinite mirrors and Droste cocoa pictures!
        //
        // If there are any cycles, the above process didn't schedule
        // the involved draw targets.
        if (result->size() < n) {
            // Schedule them now in the order they were given originally.
            for (size_t i = 0; i < n; i++) {
                if (!indexToDependencies[i].empty()) {
                    result->push_back(
                        {i, drawTargets[i], false});
                }
            }
        }

        if (result->size() != drawTargets.size()) {
            TF_CODING_ERROR("Mismatch");
        }
    }
}

// Retrieve draw targets from render index and perform topoogical sort
void
_GetSortedDrawTargets(
    HdRenderIndex *renderIndex,
    _DrawTargetEntryVector *result)
{
    HdStDrawTargetPtrVector unsortedDrawTargets;
    HdStDrawTarget::GetDrawTargets(renderIndex, &unsortedDrawTargets);
    
    _SortDrawTargets(unsortedDrawTargets, result);
}

} // namespace anonymous

void
HdxDrawTargetTask::Sync(HdSceneDelegate* delegate,
                        HdTaskContext* ctx,
                        HdDirtyBits* dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    if ((*dirtyBits) & HdChangeTracker::DirtyParams) {
        HdxDrawTargetTaskParams params;

        if (!_GetTaskParams(delegate, &params)) {
            return;
        }

        // Raster State
        // XXX: Update master raster state that is used by all passes?
        _wireframeColor          = params.wireframeColor;
        _enableLighting          = params.enableLighting;
        _overrideColor           = params.overrideColor;
        _alphaThreshold          = params.alphaThreshold;
        _cullStyle               = params.cullStyle;

        // Depth
        // XXX: Should be in raster state?
        _depthBiasUseDefault     = params.depthBiasUseDefault;
        _depthBiasEnable         = params.depthBiasEnable;
        _depthBiasConstantFactor = params.depthBiasConstantFactor;
        _depthBiasSlopeFactor    = params.depthBiasSlopeFactor;
        _depthFunc               = params.depthFunc;
    }

    if ((*dirtyBits) & HdChangeTracker::DirtyRenderTags) {
        _renderTags = _GetTaskRenderTags(delegate);
    }

    HdRenderIndex &renderIndex = delegate->GetRenderIndex();
    HdChangeTracker& changeTracker = renderIndex.GetChangeTracker();

    const unsigned drawTargetVersion
        = changeTracker.GetStateVersion(HdStDrawTargetTokens->drawTargetSet);

    if (_currentDrawTargetSetVersion != drawTargetVersion) {
        _DrawTargetEntryVector drawTargetEntries;
        _GetSortedDrawTargets(&renderIndex, &drawTargetEntries);
                              
        _renderPassesInfo.clear();
        _renderPasses.clear();

        _renderPassesInfo.reserve(drawTargetEntries.size());
        _renderPasses.reserve(drawTargetEntries.size());

        for (_DrawTargetEntry const &entry : drawTargetEntries) {
            if (HdStDrawTarget * const drawTarget = entry.drawTarget) {
                if (drawTarget->IsEnabled()) {
                    HdxDrawTargetRenderPassUniquePtr pass =
                        std::make_unique<HdxDrawTargetRenderPass>(&renderIndex);

                    pass->SetDrawTarget(drawTarget->GetGlfDrawTarget());
                    pass->SetRenderPassState(drawTarget->GetRenderPassState());
                    pass->SetHasDependentDrawTargets(
                        entry.hasDependentDrawTargets);
                    _renderPasses.push_back(std::move(pass));

                    _renderPassesInfo.push_back(
                        { std::make_shared<HdStRenderPassState>(),
                          std::make_shared<HdStSimpleLightingShader>(),
                          drawTarget,
                          drawTarget->GetVersion() });
                }
            }
        }
        _currentDrawTargetSetVersion = drawTargetVersion;
    } else {
        const size_t numRenderPasses = _renderPassesInfo.size();

        // Need to look for changes in individual draw targets.
        for (size_t renderPassIdx = 0;
             renderPassIdx < numRenderPasses;
             ++renderPassIdx) {
            _RenderPassInfo &renderPassInfo =  _renderPassesInfo[renderPassIdx];

            HdStDrawTarget const *target = renderPassInfo.target;
            const unsigned int targetVersion = target->GetVersion();

            if (renderPassInfo.version != targetVersion) {
                _renderPasses[renderPassIdx]->SetDrawTarget(
                    target->GetGlfDrawTarget());
                renderPassInfo.version = targetVersion;
            }
        }
    }

    // Store the draw targets in the task context so the resolve 
    // task does not have to extract them again.
    (*ctx)[HdxTokens->drawTargetRenderPasses] = &_renderPasses;

    ///----------------------
    static const GfMatrix4d yflip = GfMatrix4d().SetScale(
        GfVec3d(1.0, -1.0, 1.0));

    // lighting context
    GlfSimpleLightingContextRefPtr lightingContext;
    _GetTaskContextData(ctx, HdxTokens->lightingContext, &lightingContext);

    const size_t numRenderPasses = _renderPassesInfo.size();
    for (size_t renderPassIdx = 0;
         renderPassIdx < numRenderPasses;
         ++renderPassIdx) {

        _RenderPassInfo &renderPassInfo =  _renderPassesInfo[renderPassIdx];
        HdxDrawTargetRenderPass * const renderPass = 
            _renderPasses[renderPassIdx].get();
        HdStRenderPassStateSharedPtr &renderPassState = 
            renderPassInfo.renderPassState;
        HdStDrawTarget * const drawTarget = renderPassInfo.target;
        const HdStDrawTargetRenderPassState * const drawTargetRenderPassState =
            drawTarget->GetRenderPassState();
        const SdfPath &cameraId = drawTargetRenderPassState->GetCamera();

        // XXX: Need to detect when camera changes and only update if
        // needed
        const HdCamera * const camera = static_cast<const HdCamera *>(
            renderIndex.GetSprim(HdPrimTypeTokens->camera,
                cameraId));

        if (camera == nullptr) {
            // Render pass should not have been added to task list.
            TF_CODING_ERROR("Invalid camera for render pass: %s",
                            cameraId.GetText());
            return;
        }

        const HdCompareFunction depthFunc =
            HdxDrawTargetTask_GetResolvedDepthFunc(
                _depthFunc,
                drawTargetRenderPassState->GetDepthPriority());

        // Update Raster States
        renderPassState->SetOverrideColor(_overrideColor);
        renderPassState->SetWireframeColor(_wireframeColor);
        renderPassState->SetLightingEnabled(_enableLighting);
        renderPassState->SetAlphaThreshold(_alphaThreshold);
        renderPassState->SetCullStyle(_cullStyle);
        renderPassState->SetDepthFunc(depthFunc);
        renderPassState->SetAovBindings(
            drawTargetRenderPassState->GetAovBindings());

        HdStSimpleLightingShaderSharedPtr const& simpleLightingShader
            = _renderPassesInfo[renderPassIdx].simpleLightingShader;
        GlfSimpleLightingContextRefPtr const& simpleLightingContext =
            simpleLightingShader->GetLightingContext();

        renderPassState->SetLightingShader(simpleLightingShader);

        // Update camera/framing state
        // XXX Since we flip the projection matrix below, we can't set the
        // camera handle on renderPassState and use its projection matrix.
        GfVec2i const &resolution = drawTarget->GetResolution();

        GfMatrix4d const& viewMatrix = camera->GetViewMatrix();
        GfMatrix4d projectionMatrix = camera->GetProjectionMatrix();
        projectionMatrix = CameraUtilConformedWindow(projectionMatrix, 
            camera->GetWindowPolicy(),
            resolution[1] != 0.0 ? resolution[0] / resolution[1] : 1.0);
        projectionMatrix = projectionMatrix * yflip;

        const GfVec4d viewport(0, 0, resolution[0], resolution[1]);
        renderPassState->SetCameraFramingState(
            viewMatrix, projectionMatrix, viewport, camera->GetClipPlanes());

        simpleLightingContext->SetCamera(viewMatrix, projectionMatrix);

        if (lightingContext) {
            simpleLightingContext->SetUseLighting(
                lightingContext->GetUseLighting());
            simpleLightingContext->SetLights(lightingContext->GetLights());
            simpleLightingContext->SetMaterial(lightingContext->GetMaterial());
            simpleLightingContext->SetSceneAmbient(
                lightingContext->GetSceneAmbient());
            simpleLightingContext->SetShadows(lightingContext->GetShadows());
            simpleLightingContext->SetUseColorMaterialDiffuse(
                lightingContext->GetUseColorMaterialDiffuse());
        }

        renderPassState->Prepare(renderIndex.GetResourceRegistry());
        renderPass->Sync();
    }

    // XXX: Long-term Alpha to Coverage will be a render style on the
    // task.  However, as there isn't a fallback we current force it
    // enabled, unless a client chooses to manage the setting itself
    // (aka usdImaging).

    // XXX: When rendering draw targets we need alpha to coverage
    // at least until we support a transparency pass
    _enableSampleAlphaToCoverage = true;
    if (delegate->IsEnabled(HdxOptionTokens->taskSetAlphaToCoverage)) {
        if (TfDebug::IsEnabled(HDX_DISABLE_ALPHA_TO_COVERAGE)) {
            _enableSampleAlphaToCoverage = false;
        }
    }

    *dirtyBits = HdChangeTracker::Clean;
}

void
HdxDrawTargetTask::Prepare(HdTaskContext* ctx,
                           HdRenderIndex* renderIndex)
{
    const size_t numRenderPasses = _renderPassesInfo.size();
    for (size_t renderPassIdx = 0;
         renderPassIdx < numRenderPasses;
         ++renderPassIdx) {

        HdxDrawTargetRenderPass * const renderPass =
            _renderPasses[renderPassIdx].get();
        renderPass->Prepare();
    }
}

void
HdxDrawTargetTask::Execute(HdTaskContext* ctx)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // Apply polygon offset to whole pass.
    // XXX TODO: Move to an appropriate home
    if (!_depthBiasUseDefault) {
        if (_depthBiasEnable) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(_depthBiasSlopeFactor, _depthBiasConstantFactor);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    // XXX: Long-term Alpha to Coverage will be a render style on the
    // task.  However, as there isn't a fallback we current force it
    // enabled, unless a client chooses to manage the setting itself
    // (aka usdImaging).

    // XXX: When rendering draw targets we need alpha to coverage
    // at least until we support a transparency pass

    if (_enableSampleAlphaToCoverage) {
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    } else {
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }

    glEnable(GL_PROGRAM_POINT_SIZE);

    // XXX: We "Known" Hydra is always using CCW fase winding
    // which we need to flip.  This is a hack for now, but belongs in Hydra's
    // PSO.
    glFrontFace(GL_CW);

    const size_t numRenderPasses = _renderPassesInfo.size();
    for (size_t renderPassIdx = 0;
         renderPassIdx < numRenderPasses;
         ++renderPassIdx) {

        HdxDrawTargetRenderPass * const renderPass = 
            _renderPasses[renderPassIdx].get();
        HdStRenderPassStateSharedPtr const renderPassState =
            _renderPassesInfo[renderPassIdx].renderPassState;
        renderPassState->Bind();
        renderPass->Execute(renderPassState, GetRenderTags());
        renderPassState->Unbind();

        if (renderPass->HasDependentDrawTargets()) {
            // If later draw targets depend on this one, we need to
            // resolve before they fire (if MSAA enabled).
            if (renderPass->GetDrawTarget()) {
                renderPass->GetDrawTarget()->Resolve();
            }
        }
    }

    // Restore to GL defaults
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_PROGRAM_POINT_SIZE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glFrontFace(GL_CCW);
}

const TfTokenVector &
HdxDrawTargetTask::GetRenderTags() const
{
    return _renderTags;
}
// --------------------------------------------------------------------------- //
// VtValue Requirements
// --------------------------------------------------------------------------- //

std::ostream& operator<<(std::ostream& out, const HdxDrawTargetTaskParams& pv)
{
    out << "HdxDrawTargetTaskParams: (...) \n"
        << "         overrideColor           = " << pv.overrideColor << "\n"
        << "         wireframeColor          = " << pv.wireframeColor << "\n"
        << "         enableLighting          = " << pv.enableLighting << "\n"
        << "         alphaThreshold          = " << pv.alphaThreshold << "\n"
        << "         depthBiasUseDefault     = " 
            << pv.depthBiasUseDefault << "\n"
        << "         depthBiasEnable         = " << pv.depthBiasEnable << "\n"
        << "         depthBiasConstantFactor = " 
            << pv.depthBiasConstantFactor << "\n"
        << "         depthFunc               = " << pv.depthFunc << "\n"
        << "         cullStyle               = " << pv.cullStyle << "\n"
        ;

    return out;
}

bool operator==(
    const HdxDrawTargetTaskParams& lhs, 
    const HdxDrawTargetTaskParams& rhs)
{
    return 
        lhs.overrideColor == rhs.overrideColor                      && 
        lhs.wireframeColor == rhs.wireframeColor                    &&  
        lhs.enableLighting == rhs.enableLighting                    && 
        lhs.alphaThreshold == rhs.alphaThreshold                    && 
        lhs.depthBiasUseDefault == rhs.depthBiasUseDefault          && 
        lhs.depthBiasEnable == rhs.depthBiasEnable                  && 
        lhs.depthBiasConstantFactor == rhs.depthBiasConstantFactor  && 
        lhs.depthBiasSlopeFactor == rhs.depthBiasSlopeFactor        && 
        lhs.depthFunc == rhs.depthFunc                              && 
        lhs.cullStyle == rhs.cullStyle;
}

bool operator!=(
    const HdxDrawTargetTaskParams& lhs, 
    const HdxDrawTargetTaskParams& rhs)
{
    return !(lhs == rhs);
}

PXR_NAMESPACE_CLOSE_SCOPE

