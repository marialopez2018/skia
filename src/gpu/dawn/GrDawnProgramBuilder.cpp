/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/dawn/GrDawnProgramBuilder.h"

#include "include/gpu/GrRenderTarget.h"
#include "src/gpu/dawn/GrDawnGpu.h"
#include "src/sksl/SkSLCompiler.h"

#include "src/gpu/GrShaderUtils.h"

static SkSL::String sksl_to_spirv(const GrDawnGpu* gpu, const char* shaderString,
                                  SkSL::Program::Kind kind, SkSL::Program::Inputs* inputs) {
    SkSL::Program::Settings settings;
    std::unique_ptr<SkSL::Program> program = gpu->shaderCompiler()->convertProgram(
        kind,
        shaderString,
        settings);
    if (!program) {
        SkDebugf("SkSL error:\n%s\n", gpu->shaderCompiler()->errorText().c_str());
        SkASSERT(false);
        return "";
    }
    *inputs = program->fInputs;
    SkSL::String code;
    if (!gpu->shaderCompiler()->toSPIRV(*program, &code)) {
        return "";
    }
    return code;
}

static dawn::BlendFactor to_dawn_blend_factor(GrBlendCoeff coeff) {
    switch (coeff) {
    case kZero_GrBlendCoeff:
        return dawn::BlendFactor::Zero;
    case kOne_GrBlendCoeff:
        return dawn::BlendFactor::One;
    case kSC_GrBlendCoeff:
        return dawn::BlendFactor::SrcColor;
    case kISC_GrBlendCoeff:
        return dawn::BlendFactor::OneMinusSrcColor;
    case kDC_GrBlendCoeff:
        return dawn::BlendFactor::DstColor;
    case kIDC_GrBlendCoeff:
        return dawn::BlendFactor::OneMinusDstColor;
    case kSA_GrBlendCoeff:
        return dawn::BlendFactor::SrcAlpha;
    case kISA_GrBlendCoeff:
        return dawn::BlendFactor::OneMinusSrcAlpha;
    case kDA_GrBlendCoeff:
        return dawn::BlendFactor::DstAlpha;
    case kIDA_GrBlendCoeff:
        return dawn::BlendFactor::OneMinusDstAlpha;
    case kConstC_GrBlendCoeff:
        return dawn::BlendFactor::BlendColor;
    case kIConstC_GrBlendCoeff:
        return dawn::BlendFactor::OneMinusBlendColor;
    case kConstA_GrBlendCoeff:
    case kIConstA_GrBlendCoeff:
    case kS2C_GrBlendCoeff:
    case kIS2C_GrBlendCoeff:
    case kS2A_GrBlendCoeff:
    case kIS2A_GrBlendCoeff:
    default:
        SkASSERT(!"unsupported blend coefficient");
        return dawn::BlendFactor::One;
    }
}

static dawn::BlendOperation to_dawn_blend_operation(GrBlendEquation equation) {
    switch (equation) {
    case kAdd_GrBlendEquation:
        return dawn::BlendOperation::Add;
    case kSubtract_GrBlendEquation:
        return dawn::BlendOperation::Subtract;
    default:
        SkASSERT(!"unsupported blend equation");
        return dawn::BlendOperation::Add;
    }
}

static dawn::ColorStateDescriptor create_color_state(const GrDawnGpu* gpu,
                                                     const GrPipeline& pipeline,
                                                     dawn::TextureFormat colorFormat) {
    GrXferProcessor::BlendInfo blendInfo = pipeline.getXferProcessor().getBlendInfo();
    GrBlendEquation equation = blendInfo.fEquation;
    GrBlendCoeff srcCoeff = blendInfo.fSrcBlend;
    GrBlendCoeff dstCoeff = blendInfo.fDstBlend;

    dawn::BlendFactor srcFactor = to_dawn_blend_factor(srcCoeff);
    dawn::BlendFactor dstFactor = to_dawn_blend_factor(dstCoeff);
    dawn::BlendOperation operation = to_dawn_blend_operation(equation);
    auto mask = blendInfo.fWriteColor ? dawn::ColorWriteMask::All : dawn::ColorWriteMask::None;

    dawn::BlendDescriptor colorDesc = {operation, srcFactor, dstFactor};
    dawn::BlendDescriptor alphaDesc = {operation, srcFactor, dstFactor};

    dawn::ColorStateDescriptor descriptor;
    descriptor.format = colorFormat;
    descriptor.alphaBlend = alphaDesc;
    descriptor.colorBlend = colorDesc;
    descriptor.nextInChain = nullptr;
    descriptor.writeMask = mask;

    return descriptor;
}

static dawn::BindGroupBinding make_bind_group_binding(uint32_t binding, const dawn::Buffer& buffer,
                                                      uint32_t offset, uint32_t size, const
                                                      dawn::Sampler& sampler,
                                                      const dawn::TextureView& textureView) {
    dawn::BindGroupBinding result;
    result.binding = binding;
    result.buffer = buffer;
    result.offset = offset;
    result.size = size;
    result.sampler = sampler;
    result.textureView = textureView;
    return result;
}

static dawn::BindGroupBinding make_bind_group_binding(uint32_t binding, const dawn::Buffer& buffer,
                                                      uint32_t offset, uint32_t size) {
    return make_bind_group_binding(binding, buffer, offset, size, nullptr, nullptr);
}

sk_sp<GrDawnProgram> GrDawnProgramBuilder::Build(GrDawnGpu* gpu,
                                                 GrRenderTarget* renderTarget,
                                                 GrSurfaceOrigin origin,
                                                 const GrPipeline& pipeline,
                                                 const GrPrimitiveProcessor& primProc,
                                                 const GrTextureProxy* const primProcProxies[],
                                                 dawn::TextureFormat colorFormat,
                                                 GrProgramDesc* desc) {
    GrDawnProgramBuilder builder(gpu, renderTarget, origin, primProc, primProcProxies, pipeline,
                                 desc);
    if (!builder.emitAndInstallProcs()) {
        return nullptr;
    }

    builder.fVS.extensions().appendf("#extension GL_ARB_separate_shader_objects : enable\n");
    builder.fFS.extensions().appendf("#extension GL_ARB_separate_shader_objects : enable\n");
    builder.fVS.extensions().appendf("#extension GL_ARB_shading_language_420pack : enable\n");
    builder.fFS.extensions().appendf("#extension GL_ARB_shading_language_420pack : enable\n");

    builder.finalizeShaders();

    SkSL::Program::Inputs vertInputs, fragInputs;
    GrDawnUniformHandler::UniformInfoArray& uniforms = builder.fUniformHandler.fUniforms;
    uint32_t geometryUniformSize = builder.fUniformHandler.fCurrentGeometryUBOOffset;
    uint32_t fragmentUniformSize = builder.fUniformHandler.fCurrentFragmentUBOOffset;
    sk_sp<GrDawnProgram> result(
        new GrDawnProgram(uniforms, geometryUniformSize, fragmentUniformSize));
    result->fVSModule = builder.createShaderModule(builder.fVS, SkSL::Program::kVertex_Kind,
                                                   &vertInputs);
    result->fFSModule = builder.createShaderModule(builder.fFS, SkSL::Program::kFragment_Kind,
                                                   &fragInputs);
    result->fGeometryProcessor = std::move(builder.fGeometryProcessor);
    result->fXferProcessor = std::move(builder.fXferProcessor);
    result->fFragmentProcessors = std::move(builder.fFragmentProcessors);
    result->fFragmentProcessorCnt = builder.fFragmentProcessorCnt;
    std::vector<dawn::BindGroupLayoutBinding> layoutBindings;
    std::vector<dawn::BindGroupBinding> bindings;

    if (0 != geometryUniformSize) {
        dawn::BufferDescriptor desc;
        desc.usage = dawn::BufferUsageBit::Uniform | dawn::BufferUsageBit::CopyDst;
        desc.size = geometryUniformSize;
        result->fGeometryUniformBuffer = gpu->device().CreateBuffer(&desc);
        bindings.push_back(make_bind_group_binding(0, result->fGeometryUniformBuffer, 0,
                                                   geometryUniformSize));
        layoutBindings.push_back({ 0, dawn::ShaderStageBit::Vertex,
                                   dawn::BindingType::UniformBuffer});
    }
    if (0 != fragmentUniformSize) {
        dawn::BufferDescriptor desc;
        desc.usage = dawn::BufferUsageBit::Uniform | dawn::BufferUsageBit::CopyDst;
        desc.size = fragmentUniformSize;
        result->fFragmentUniformBuffer = gpu->device().CreateBuffer(&desc);
        bindings.push_back(make_bind_group_binding(1, result->fFragmentUniformBuffer, 0,
                                                   fragmentUniformSize));
        layoutBindings.push_back({ 1, dawn::ShaderStageBit::Fragment,
                                   dawn::BindingType::UniformBuffer});
    }
    dawn::BindGroupLayoutDescriptor bindGroupLayoutDesc;
    bindGroupLayoutDesc.bindingCount = layoutBindings.size();
    bindGroupLayoutDesc.bindings = layoutBindings.data();
    auto bindGroupLayout = gpu->device().CreateBindGroupLayout(&bindGroupLayoutDesc);
    dawn::BindGroupDescriptor descriptor;
    descriptor.layout = bindGroupLayout;
    descriptor.bindingCount = bindings.size();
    descriptor.bindings = bindings.data();
    result->fUniformBindGroup = gpu->device().CreateBindGroup(&descriptor);
    dawn::PipelineLayoutDescriptor pipelineLayoutDesc;
    pipelineLayoutDesc.bindGroupLayoutCount = 1;
    pipelineLayoutDesc.bindGroupLayouts = &bindGroupLayout;
    result->fPipelineLayout = gpu->device().CreatePipelineLayout(&pipelineLayoutDesc);
    result->fBuiltinUniformHandles = builder.fUniformHandles;
    result->fColorState = create_color_state(gpu, pipeline, colorFormat);
    return result;
}

GrDawnProgramBuilder::GrDawnProgramBuilder(GrDawnGpu* gpu,
                                           GrRenderTarget* renderTarget,
                                           GrSurfaceOrigin origin,
                                           const GrPrimitiveProcessor& primProc,
                                           const GrTextureProxy* const primProcProxies[],
                                           const GrPipeline& pipeline,
                                           GrProgramDesc* desc)
    : INHERITED(renderTarget, origin, primProc, primProcProxies, pipeline, desc)
    , fGpu(gpu)
    , fVaryingHandler(this)
    , fUniformHandler(this) {
}

dawn::ShaderModule GrDawnProgramBuilder::createShaderModule(const GrGLSLShaderBuilder& builder,
                                                            SkSL::Program::Kind kind,
                                                            SkSL::Program::Inputs* inputs) {
    dawn::Device device = fGpu->device();
    SkString source(builder.fCompilerString.c_str());

#if 0
    SkSL::String sksl = GrShaderUtils::PrettyPrint(builder.fCompilerString);
    printf("converting program:\n%s\n", sksl.c_str());
#endif

    SkSL::String spirvSource = sksl_to_spirv(fGpu, source.c_str(), kind, inputs);

    dawn::ShaderModuleDescriptor desc;
    desc.codeSize = spirvSource.size() / 4;
    desc.code = reinterpret_cast<const uint32_t*>(spirvSource.c_str());

    return device.CreateShaderModule(&desc);
};

const GrCaps* GrDawnProgramBuilder::caps() const {
    return fGpu->caps();
}

void GrDawnProgram::setRenderTargetState(const GrRenderTarget* rt, GrSurfaceOrigin origin) {
    // Load the RT height uniform if it is needed to y-flip gl_FragCoord.
    if (fBuiltinUniformHandles.fRTHeightUni.isValid() &&
        fRenderTargetState.fRenderTargetSize.fHeight != rt->height()) {
        fDataManager.set1f(fBuiltinUniformHandles.fRTHeightUni, SkIntToScalar(rt->height()));
    }

    // set RT adjustment
    SkISize size;
    size.set(rt->width(), rt->height());
    SkASSERT(fBuiltinUniformHandles.fRTAdjustmentUni.isValid());
    if (fRenderTargetState.fRenderTargetOrigin != origin ||
        fRenderTargetState.fRenderTargetSize != size) {
        fRenderTargetState.fRenderTargetSize = size;
        fRenderTargetState.fRenderTargetOrigin = origin;

        float rtAdjustmentVec[4];
        fRenderTargetState.getRTAdjustmentVec(rtAdjustmentVec);
        fDataManager.set4fv(fBuiltinUniformHandles.fRTAdjustmentUni, 1, rtAdjustmentVec);
    }
}

void GrDawnProgram::setData(const GrPrimitiveProcessor& primProc,
                            const GrRenderTarget* renderTarget,
                            GrSurfaceOrigin origin,
                            const GrPipeline& pipeline) {
    this->setRenderTargetState(renderTarget, origin);
    fGeometryProcessor->setData(fDataManager, primProc,
                                GrFragmentProcessor::CoordTransformIter(pipeline));
    GrFragmentProcessor::Iter iter(pipeline);
    GrGLSLFragmentProcessor::Iter glslIter(fFragmentProcessors.get(), fFragmentProcessorCnt);
    const GrFragmentProcessor* fp = iter.next();
    GrGLSLFragmentProcessor* glslFP = glslIter.next();
    while (fp && glslFP) {
        glslFP->setData(fDataManager, *fp);
        fp = iter.next();
        glslFP = glslIter.next();
    }
    fDataManager.uploadUniformBuffers(fGeometryUniformBuffer,
                                      fFragmentUniformBuffer);
}
