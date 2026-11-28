#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""DirectX-side feature matrix: what DXVK (D3D8/9/10/11) and vkd3d-proton (D3D12) need from a Vulkan driver, against
`vulkaninfo --json` (or vulkaninfo text) captures of our ICD.  Stdlib only.  Self-contained: it shares no code with
tools/feature-matrix/ (plain Vulkan lane).

  fmdx.py extract --refs DIR [--mesa DIR]     re-extract data/extracted.json from the upstream clones (see REFS below)
  fmdx.py check CAPTURE... [--expect K=N ...] per consumer and level: PASS or the missing list; --expect fails on a count change
  fmdx.py markdown CAPTURE_v10 CAPTURE_v11 CAPTURE_v13 > vk/FEATURE-MATRIX-DIRECTX.md
  fmdx.py selftest                            falsification on the committed captures (a flipped bit must move the count)

A CAPTURE is `vulkaninfo --json=0 --output f.json` (profile format) or plain `vulkaninfo` text (phones; no format section).
Every requirement carries its source as path:line at the commit in REFS.  Curated rows are below; DXVK's own feature table,
DXGI format table and PanVK's advertised values are extracted mechanically into data/extracted.json by `extract`.
"""
import contextlib, copy, io, json, os, re, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
REFS = {  # clone name -> (upstream, commit) ; clones live in /tmp/opencode/feature-refs-dx/<name>
    "dxvk-master": ("https://github.com/doitsujin/dxvk", "1011dd97474a1c8c2608f3466429e4df45b36b7c"),  # = v3.2, 2026-11-26
    "dxvk-3.1.1": ("https://github.com/doitsujin/dxvk", "b1a1c99ab52b687cf950d62c88bc2fa316b41663"),   # Proton/bridge DXVK
    "dxvk-2.7.1": ("https://github.com/doitsujin/dxvk", "c3dd74be6baec53786d4e064a572185b70347a17"),   # pinned by Mali launchers
    "vkd3d-proton": ("https://github.com/HansKristian-Work/vkd3d-proton", "71cae091b06571e51446cf2a937f0b04c237c04d"),
    "mesa": ("third_party/mesa (MESA-SOURCE.txt)", "0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4"),        # PanVK oracle, 26.3.0
}
DXVK_VERSIONS = ["dxvk-2.7.1", "dxvk-3.1.1", "dxvk-master"]

# ---------------------------------------------------------------------------------------------------------------------
# Curated requirements.  check atoms:
#   F:member            a VkBool32 feature (any feature struct), X:VK_ext an extension (KHR/EXT spellings are aliases),
#   P:Struct.a.b OP v   a property (Struct = VkPhysicalDeviceProperties / ...Vulkan12Properties / ...), OP in >= <= == !=
#   API>=1.3, Q:SPARSE (a queue family with SPARSE_BINDING), SG:OPS=a,b;STAGES=c,d (Vulkan11Properties subgroup bits),
#   FMT:VK_FORMAT_X:BIT,BIT (optimal or linear tiling has all bits), BUF:VK_FORMAT_X:BIT (buffer features).
#   A '|' inside one atom string means any-of.  All atoms of a row must hold.
# consumer/level: dxvk-<ver>/device (hard = no D3D device at all), d3d11/<FL> (cumulative ladder, DXVK master),
#   d3d9/optional, dxgi/format, vkd3d/device, vkd3d/<FL>, vkd3d/SM<x>, vkd3d/optional.
V = "vkd3d-proton/libs/vkd3d/"
DM = "dxvk-master/src/"
CURATED = [
    # --- DXVK device creation (beyond the ENABLE_* table, which is extracted) ---
    dict(id="dxvk-api-1.3", consumer="dxvk-*", level="device", hard=True, check=["API>=1.3"],
         cite=DM + "dxvk/dxvk_instance.h:12, dxvk/dxvk_device_info.cpp:780", effect="adapter skipped"),
    dict(id="dxvk-push-256", consumer="dxvk-*", level="device", hard=True,
         check=["P:VkPhysicalDeviceProperties.limits.maxPushConstantsSize>=256"],
         cite=DM + "dxvk/dxvk_device_info.cpp:800, dxvk/dxvk_limits.h:22 (3.1.1 :767, 2.7.1 :650)",
         effect="'Device does not support 256 of push data' (waived only with VK_EXT_descriptor_heap)"),
    # --- D3D11 feature-level ladder (DXVK master; same gates in 3.1.1 per notes/route-decision-2.md) ---
    *[dict(id="d3d11-11_0-" + f, consumer="d3d11", level="11_0", hard=True, check=["F:" + f],
           cite=DM + "d3d11/d3d11_features.cpp:193-197", effect="max feature level 10_1: games that ask for 11_0 fail D3D11CreateDevice")
      for f in ("drawIndirectFirstInstance", "fragmentStoresAndAtomics", "multiDrawIndirect", "tessellationShader")],
    dict(id="d3d11-11_1-logicOp", consumer="d3d11", level="11_1", hard=True, check=["F:logicOp"],
         cite=DM + "d3d11/d3d11_features.cpp:54,200", effect="OutputMergerLogicOp FALSE -> max FL 11_0"),
    dict(id="d3d11-11_1-vpsa", consumer="d3d11", level="11_1", hard=True, check=["F:vertexPipelineStoresAndAtomics"],
         cite=DM + "d3d11/d3d11_features.cpp:201", effect="UAVs in VS/HS/DS/GS -> max FL 11_0"),
    dict(id="d3d11-12_0-tiled2", consumer="d3d11", level="12_0", hard=True,
         check=["F:sparseBinding", "F:sparseResidencyBuffer", "F:sparseResidencyImage2D", "F:sparseResidencyAliased",
                "P:VkPhysicalDeviceProperties.sparseProperties.residencyStandard2DBlockShape==True",
                "F:shaderResourceResidency", "F:shaderResourceMinLod", "F:samplerFilterMinmax",
                "P:VkPhysicalDeviceVulkan12Properties.filterMinmaxSingleComponentFormats==True",
                "P:VkPhysicalDeviceProperties.sparseProperties.residencyNonResidentStrict==True",
                "P:VkPhysicalDeviceProperties.sparseProperties.residencyAlignedMipSize==False"],
         cite=DM + "d3d11/d3d11_features.cpp:205,319-347", effect="TiledResourcesTier < 2 -> max FL 11_1"),
    dict(id="d3d11-12_0-typeduav", consumer="d3d11", level="12_0", hard=True,
         check=["F:shaderStorageImageReadWithoutFormat"] + ["FMT:%s:VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT" % f for f in (
             "VK_FORMAT_R32_SFLOAT VK_FORMAT_R32_UINT VK_FORMAT_R32_SINT VK_FORMAT_R32G32B32A32_SFLOAT VK_FORMAT_R32G32B32A32_UINT "
             "VK_FORMAT_R32G32B32A32_SINT VK_FORMAT_R16G16B16A16_SFLOAT VK_FORMAT_R16G16B16A16_UINT VK_FORMAT_R16G16B16A16_SINT "
             "VK_FORMAT_R8G8B8A8_UNORM VK_FORMAT_R8G8B8A8_UINT VK_FORMAT_R8G8B8A8_SINT VK_FORMAT_R16_SFLOAT VK_FORMAT_R16_UINT "
             "VK_FORMAT_R16_SINT VK_FORMAT_R8_UNORM VK_FORMAT_R8_UINT VK_FORMAT_R8_SINT").split()],
         cite=DM + "d3d11/d3d11_features.cpp:206,350-388",
         effect="TypedUAVLoadAdditionalFormats FALSE -> max FL 11_1 (proxy: STORAGE_READ_WITHOUT_FORMAT is a FormatProperties3 bit "
                "vulkaninfo JSON does not carry; checked as the feature + STORAGE_IMAGE on all 18 formats)"),
    dict(id="d3d11-12_1-conservative", consumer="d3d11", level="12_1", hard=True, check=["X:VK_EXT_conservative_rasterization"],
         cite=DM + "d3d11/d3d11_features.cpp:210,218-238", effect="ConservativeRasterizationTier 0 -> max FL 12_0"),
    dict(id="d3d11-12_1-rov", consumer="d3d11", level="12_1", hard=True, check=["F:fragmentShaderPixelInterlock"],
         cite=DM + "d3d11/d3d11_features.cpp:84,211", effect="ROVsSupported FALSE -> max FL 12_0"),
    dict(id="d3d11-doubles", consumer="d3d11", level="optional", hard=False, check=["F:shaderFloat64", "F:shaderInt64"],
         cite=DM + "d3d11/d3d11_features.cpp:40-41,61,103", effect="no D3D11 double-precision shader ops (games check the cap)"),
    dict(id="d3d11-minprec16", consumer="d3d11", level="optional", hard=False, check=["F:shaderInt16", "F:shaderFloat16"],
         cite=DM + "d3d11/d3d11_features.cpp:118-123", effect="min16float runs at 32 bit (perf only)"),
    dict(id="d3d11-vprt-any-stage", consumer="d3d11", level="optional", hard=False, check=["F:shaderOutputViewportIndex", "F:shaderOutputLayer"],
         cite=DM + "d3d11/d3d11_features.cpp:90-92", effect="SV_ViewportArrayIndex/SV_RenderTargetArrayIndex only from a GS"),
    dict(id="d3d11-stencil-export", consumer="d3d11", level="optional", hard=False, check=["X:VK_EXT_shader_stencil_export"],
         cite=DM + "d3d11/d3d11_features.cpp:85", effect="PSSpecifiedStencilRefSupported FALSE"),
    # --- D3D9 (DXVK device + these) ---
    dict(id="d3d9-feedback-loop", consumer="d3d9", level="optional", hard=False, check=["F:attachmentFeedbackLoopLayout"],
         cite=DM + "d3d9/d3d9_device.cpp:125, d3d9/d3d9_adapter.cpp:324", effect="render-target-as-texture hazards use barriers/copies"),
    dict(id="d3d9-depth-bias-control", consumer="d3d9", level="optional", hard=False, check=["F:depthBiasControl"],
         cite=DM + "d3d9/d3d9_device.cpp:103-111", effect="D3D9 depth bias scaled heuristically (z-fighting in some games)"),
    dict(id="d3d9-swvp", consumer="d3d9", level="optional", hard=False, check=["F:vertexPipelineStoresAndAtomics"],
         cite=DM + "d3d9/d3d9_device.cpp:4759", effect="ProcessVertices / software vertex processing unavailable"),
    dict(id="d3d9-large-points", consumer="d3d9", level="optional", hard=False,
         check=["F:largePoints", "P:VkPhysicalDeviceProperties.limits.pointSizeRange.1>=64"],
         cite=DM + "d3d9/d3d9_adapter.cpp:687", effect="D3DCAPS9.MaxPointSize = pointSizeRange[1]; point sprites clamp"),
    dict(id="d3d9-d16s8", consumer="d3d9", level="optional", hard=False,
         check=["FMT:VK_FORMAT_D16_UNORM_S8_UINT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT"],
         cite=DM + "d3d9/d3d9_format.cpp:505", effect="D3DFMT_D15S1/D16S8 style formats unavailable"),
    # --- DXGI/D3D9 depth fallbacks (no device failure, but one of the pair must hold) ---
    dict(id="fmt-d24s8-or-d32s8", consumer="dxgi", level="format", hard=True,
         check=["FMT:VK_FORMAT_D24_UNORM_S8_UINT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"
                "|FMT:VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"],
         cite=DM + "dxgi/dxgi_format.cpp:864-872, d3d9/d3d9_format.cpp:498", effect="no D24S8 depth-stencil at all (every 3D game)"),
    dict(id="fmt-d24s8-native", consumer="dxgi", level="format", hard=False,
         check=["FMT:VK_FORMAT_D24_UNORM_S8_UINT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"],
         cite=DM + "dxgi/dxgi_format.cpp:864", effect="D24S8 -> D32_SFLOAT_S8 (2x depth bandwidth, D3D9 lockable depth emulated)"),
    dict(id="fmt-a8-native", consumer="dxgi", level="format", hard=False,
         check=["FMT:VK_FORMAT_A8_UNORM_KHR:VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT"],
         cite=DM + "dxgi/dxgi_format.cpp:874-882", effect="A8_UNORM -> R8_UNORM + swizzle (fine)"),
    # --- vkd3d-proton device creation: vkd3d_init_device_caps() returns E_INVALIDARG, bindless init E_NOTIMPL ---
    dict(id="vkd3d-api-1.3", consumer="vkd3d", level="device", hard=True, check=["API>=1.3"],
         cite="vkd3d-proton/include/vkd3d.h:53, " + V + "device.c:2746", effect="adapter skipped"),
    dict(id="vkd3d-divisor", consumer="vkd3d", level="device", hard=True,
         check=["F:vertexAttributeInstanceRateDivisor", "F:vertexAttributeInstanceRateZeroDivisor"],
         cite=V + "device.c:2510-2515", effect="'Lacking support for VK_EXT_vertex_attribute_divisor'"),
    dict(id="vkd3d-xfb-queries", consumer="vkd3d", level="device", hard=True,
         check=["P:VkPhysicalDeviceTransformFeedbackPropertiesEXT.transformFeedbackQueries==True"],
         cite=V + "device.c:2517-2521", effect="'Lacking support for transform feedback'"),
    dict(id="vkd3d-single-texel", consumer="vkd3d", level="device", hard=True,
         check=["P:VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetSingleTexelAlignment==True"
                "|P:VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetAlignmentBytes==1",
                "P:VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetSingleTexelAlignment==True"
                "|P:VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetAlignmentBytes==1"],
         cite=V + "device.c:2523-2536", effect="'Lacking support for single texel alignment'"),
    dict(id="vkd3d-mirror-clamp", consumer="vkd3d", level="device", hard=True, check=["F:samplerMirrorClampToEdge"],
         cite=V + "device.c:2634-2638", effect="E_INVALIDARG"),
    dict(id="vkd3d-robustness2", consumer="vkd3d", level="device", hard=True, check=["F:robustBufferAccess2", "F:robustImageAccess2"],
         cite=V + "device.c:2640-2645", effect="'Robustness2 features not supported'"),
    dict(id="vkd3d-null-descriptor", consumer="vkd3d", level="device", hard=True, check=["F:nullDescriptor"],
         cite=V + "device.c:2647-2651", effect="E_INVALIDARG"),
    dict(id="vkd3d-draw-params", consumer="vkd3d", level="device", hard=True, check=["F:shaderDrawParameters"],
         cite=V + "device.c:2656-2660", effect="E_INVALIDARG"),
    dict(id="vkd3d-push-descriptor", consumer="vkd3d", level="device", hard=True, check=["X:VK_KHR_push_descriptor"],
         cite=V + "device.c:2662-2666", effect="'Push descriptors are not supported'"),
    dict(id="vkd3d-maint56", consumer="vkd3d", level="device", hard=True, check=["F:maintenance5", "F:maintenance6"],
         cite=V + "device.c:2668-2673", effect="E_INVALIDARG"),
    *[dict(id="vkd3d-bindless-" + f, consumer="vkd3d", level="device", hard=True, check=["F:" + f],
           cite=V + "state.c:8294-8301", effect="'Insufficient descriptor indexing support' (E_NOTIMPL)")
      for f in ("descriptorIndexing", "shaderStorageTexelBufferArrayNonUniformIndexing",
                "shaderStorageImageArrayNonUniformIndexing", "descriptorBindingVariableDescriptorCount")],
    *[dict(id="vkd3d-uab-" + n, consumer="vkd3d", level="device", hard=True,
           check=["P:VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBind%s>=1000000|X:VK_EXT_descriptor_buffer" % n],
           cite=V + "state.c:8151-8156, vkd3d_private.h:67 (1,000,000 = D3D12 heap tier 2)",
           effect="legacy (non descriptor-buffer) bindless path fails: 'Insufficient descriptor indexing support'")
      for n in ("SampledImages", "StorageImages", "StorageBuffers")],
    dict(id="vkd3d-cbv-ubo", consumer="vkd3d", level="optional", hard=False,
         check=["P:VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers>=1000000",
                "F:descriptorBindingUniformBufferUpdateAfterBind", "F:shaderUniformBufferArrayNonUniformIndexing"],
         cite=V + "state.c:7764-7767", effect="CBVs bound as SSBOs (VKD3D_BINDLESS_CBV_AS_SSBO; slower)"),
    dict(id="vkd3d-raw-ssbo", consumer="vkd3d", level="optional", hard=False,
         check=["P:VkPhysicalDeviceProperties.limits.minStorageBufferOffsetAlignment<=16"],
         cite=V + "state.c:7772-7775", effect="no RAW_SSBO path: ByteAddressBuffers through texel buffers"),
    dict(id="vkd3d-mutable", consumer="vkd3d", level="optional", hard=False,
         check=["X:VK_EXT_mutable_descriptor_type|X:VK_VALVE_mutable_descriptor_type"],
         cite=V + "state.c:7687,7824", effect="one descriptor set per type: 3-4x descriptor memory and copies"),
    dict(id="vkd3d-descriptor-buffer", consumer="vkd3d", level="optional", hard=False, check=["X:VK_EXT_descriptor_buffer"],
         cite=V + "state.c:7761", effect="legacy descriptor-set heap (needs the 1M UAB counts instead)"),
    dict(id="vkd3d-image-view-min-lod", consumer="vkd3d", level="optional", hard=False, check=["F:minLod"],
         cite=V + "device.c:117", effect="ResourceMinLODClamp ignored"),
    # --- vkd3d-proton feature-level ladder (device.c d3d12_device_caps_init_feature_level) ---
    dict(id="vkd3d-11_1", consumer="vkd3d", level="11_1", hard=True,
         check=["F:logicOp", "F:vertexPipelineStoresAndAtomics",
                "P:VkPhysicalDeviceProperties.limits.maxPerStageDescriptorStorageBuffers>=64",
                "P:VkPhysicalDeviceProperties.limits.maxPerStageDescriptorStorageImages>=64"],
         cite=V + "device.c:9708-9713,9319", effect="max FL 11_0"),
    dict(id="vkd3d-12_0-tiled2", consumer="vkd3d", level="12_0", hard=True,
         check=["F:sparseBinding", "F:sparseResidencyAliased", "F:sparseResidencyBuffer", "F:sparseResidencyImage2D", "Q:SPARSE",
                "P:VkPhysicalDeviceProperties.sparseProperties.residencyStandard2DBlockShape==True",
                "F:shaderResourceResidency", "F:shaderResourceMinLod",
                "P:VkPhysicalDeviceProperties.sparseProperties.residencyAlignedMipSize==False",
                "P:VkPhysicalDeviceProperties.sparseProperties.residencyNonResidentStrict==True",
                "P:VkPhysicalDeviceVulkan12Properties.filterMinmaxSingleComponentFormats==True"],
         cite=V + "device.c:9715-9719,8994-9015", effect="TiledResourcesTier < 2 -> max FL 11_1 (most D3D12 games ask 12_0)"),
    dict(id="vkd3d-12_0-typeduav", consumer="vkd3d", level="12_0", hard=True,
         check=["F:shaderStorageImageReadWithoutFormat"] + ["FMT:%s:VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT" % f for f in (
             "VK_FORMAT_R32_SFLOAT VK_FORMAT_R32G32B32A32_SFLOAT VK_FORMAT_R16G16B16A16_SFLOAT VK_FORMAT_R8G8B8A8_UNORM "
             "VK_FORMAT_R16_SFLOAT VK_FORMAT_R8_UNORM VK_FORMAT_R8_UINT VK_FORMAT_R16_UINT").split()],
         cite=V + "device.c:9159-9195", effect="TypedUAVLoadAdditionalFormats FALSE -> max FL 11_1 (proxy as for D3D11)"),
    dict(id="vkd3d-12_1", consumer="vkd3d", level="12_1", hard=True,
         check=["F:fragmentShaderPixelInterlock", "F:fragmentShaderSampleInterlock", "X:VK_EXT_conservative_rasterization"],
         cite=V + "device.c:9721-9723,9327-9329,9020", effect="max FL 12_0"),
    dict(id="vkd3d-12_2", consumer="vkd3d", level="12_2", hard=True,
         check=["F:shaderOutputViewportIndex", "F:shaderOutputLayer", "F:depthBounds", "X:VK_EXT_mesh_shader",
                "X:VK_KHR_ray_tracing_pipeline", "X:VK_KHR_fragment_shading_rate", "F:sparseResidencyImage3D"],
         cite=V + "device.c:9725-9736", effect="no DX12 Ultimate (SM6.5, RT 1.1, VRS 2, mesh, sampler feedback)"),
    # --- vkd3d-proton shader models (DXIL games need >= 6.0) ---
    dict(id="vkd3d-sm6_0", consumer="vkd3d", level="SM6.0", hard=True,
         check=["P:VkPhysicalDeviceVulkan11Properties.subgroupSize>=4",
                "SG:OPS=VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,VK_SUBGROUP_FEATURE_BASIC_BIT,VK_SUBGROUP_FEATURE_BALLOT_BIT,"
                "VK_SUBGROUP_FEATURE_SHUFFLE_BIT,VK_SUBGROUP_FEATURE_QUAD_BIT,VK_SUBGROUP_FEATURE_VOTE_BIT;"
                "STAGES=VK_SHADER_STAGE_COMPUTE_BIT,VK_SHADER_STAGE_FRAGMENT_BIT",
                "F:scalarBlockLayout|F:uniformBufferStandardLayout", "F:shaderInt16"],
         cite=V + "device.c:9799-9832", effect="max SM 5.1: every DXIL (SM6) title fails pipeline creation"),
    dict(id="vkd3d-sm6_2", consumer="vkd3d", level="SM6.2", hard=True,
         check=["P:VkPhysicalDeviceVulkan12Properties.denormBehaviorIndependence!=VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE",
                "P:VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat32==True",
                "P:VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat32==True"],
         cite=V + "device.c:9848-9861", effect="max SM 6.0 (SM 6.1/6.2 titles, native 16-bit)"),
    dict(id="vkd3d-sm6_6", consumer="vkd3d", level="SM6.6", hard=True,
         check=["F:computeDerivativeGroupLinear", "F:shaderBufferInt64Atomics", "F:shaderInt8"],
         cite=V + "device.c:9912-9917", effect="max SM 6.5 (UE5 titles need SM 6.6)"),
]

# Hand notes on the hardware (Valhall v10/v11, 5th gen v13) per atom; HYPOTHESIS unless a test is named.
HW = {
    "geometryShader": "no GS stage in Valhall; emulate in compute (Mesa poly lowering, as Asahi/Honeykrisp; repo PR #378/#392)",
    "tessellationShader": "no tessellator; emulate in compute (Mesa poly tess lowering; repo PR #385/#388)",
    "shaderCullDistance": "no HW cull planes; lower to per-primitive 0/1 varying + discard or GS-less prim cull (PR #350)",
    "descriptorIndexing": "descriptor tables are memory: all types can be indexed/UAB; PanVK does it on v9+",
    "fillModeNonSolid": "no polygon-mode HW: lower to lines/points in a compute or index rewrite; DXVK hard req",
    "multiViewport": "single viewport per draw in HW; multi-viewport needs per-viewport replay or GS-like routing",
    "robustImageAccess2": "texture descriptors clamp; OOB fetch needs bounds check returning (0,0,0,0/1)",
    "logicOp": "blend shader can do logic ops (PanVK sets logicOp=true)",
    "vertexPipelineStoresAndAtomics": "IDVS may run VS speculatively/twice; PanVK only v13 (drirc); see PR #372",
    "sparseResidencyAliased": "PanVK leaves it false (panfrost issue 237); GPU VA aliasing should allow it",
    "fragmentShaderPixelInterlock": "no known ordered-raster HW interface on Valhall (HYPOTHESIS)",
    "shaderFloat64": "no fp64 ALU; would be soft-float (slow); do not advertise",
    "depthBounds": "no depth-bounds HW in the ZSD (HYPOTHESIS); emulate with discard",
    "VK_KHR_push_descriptor": "pure driver work (descriptor set in the command buffer)",
    "VK_EXT_conservative_rasterization": "PanVK v11+ only",
    "transformFeedbackQueries": "XFB is NIR-lowered VS buffer stores (feat/xfb-emu); queries = counters in the same pass",
    "drawIndirectFirstInstance": "advertised",
    "multiDrawIndirect": "CSF can loop indirect draws (PanVK v10+)",
}


# ---------------------------------------------------------------------------------------------------------------------
# Capture loading
def load_capture(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        txt = f.read()
    if txt.lstrip().startswith("{"):
        d = json.loads(txt)["capabilities"]["device"]
        return dict(ext=set(d.get("extensions", {})), ext_complete=bool(d.get("extensions")), feat=d.get("features", {}), prop=d.get("properties", {}),
                    fmt=d.get("formats", {}), queues=[q.get("VkQueueFamilyProperties", q) for q in d.get("queueFamiliesProperties", [])],
                    src=path, has_formats=bool(d.get("formats")))
    return parse_text(txt, path)


def _val(s):
    s = s.strip()
    if s in ("true", "false"):
        return s == "true"
    if re.match(r"^0x[0-9a-fA-F]+$", s):
        return int(s, 16)
    if re.match(r"^-?\d+\.\d+$", s):
        return float(s)
    m = re.match(r"^(-?\d+)\b", s)
    if m and not re.match(r"^\d+\.\d+\.\d+", s):
        return int(m.group(1))
    return s


def parse_text(txt, path):
    """Plain vulkaninfo text (first GPU only)."""
    feat, prop, ext, queues = {}, {"VkPhysicalDeviceProperties": {"limits": {}, "sparseProperties": {}}}, set(), []
    cur, listkey, section = None, None, None
    for line in txt.splitlines():
        if line.startswith("GPU1:"):
            break
        m = re.match(r"^(VkPhysicalDevice\w+|VkQueueFamilyProperties):\s*$", line)
        if m:
            name, section, listkey = m.group(1), None, None
            if name == "VkPhysicalDeviceLimits":
                cur = prop["VkPhysicalDeviceProperties"]["limits"]
            elif name == "VkPhysicalDeviceSparseProperties":
                cur = prop["VkPhysicalDeviceProperties"]["sparseProperties"]
            elif name == "VkPhysicalDeviceProperties":
                cur = prop["VkPhysicalDeviceProperties"]
            elif "Features" in name:
                cur = feat.setdefault(name, {})
            elif name == "VkQueueFamilyProperties":
                cur = None; section = "queues"
            else:
                cur = prop.setdefault(name, {})
            continue
        if line.startswith("Device Extensions:"):
            section, cur = "ext", None
            continue
        if section == "ext":
            m = re.match(r"^\s+(VK_\w+)\s*:", line)
            if m:
                ext.add(m.group(1)); continue
            if line.strip() and not line.startswith("\t"):
                section = None
            continue
        if section == "queues":
            m = re.match(r"^\s+queueFlags\s*=\s*(.+)$", line)
            if m:
                queues.append({"queueFlags": ["VK_" + t.strip() for t in m.group(1).split("|")]})
            continue
        if cur is None:
            continue
        m = re.match(r"^\s+(\w+):\s*count\s*=\s*\d+", line)
        if m:
            listkey = m.group(1); cur[listkey] = []; continue
        m = re.match(r"^\s+(\w+)\s*=\s*(.+)$", line)
        if m:
            listkey = None
            k, v = m.group(1), m.group(2)
            if k == "apiVersion":
                mm = re.match(r"(\d+)\.(\d+)\.(\d+)", v)
                v = (int(mm.group(1)) << 22) | (int(mm.group(2)) << 12) | int(mm.group(3)) if mm else v
            elif k in ("driverID", "denormBehaviorIndependence", "roundingModeIndependence"):
                v = "VK_" + v.strip()
            else:
                v = _val(v)
            cur[k] = v
            continue
        m = re.match(r"^\s+([A-Z][A-Z0-9_]+|-?[\d.]+)\s*$", line)
        if m and listkey:
            t = m.group(1)
            cur[listkey].append(_val(t) if t[0] in "-0123456789" else "VK_" + t)
    return dict(ext=ext, ext_complete=False, feat=feat, prop=prop, fmt={}, queues=queues, src=path, has_formats=False)


# ---------------------------------------------------------------------------------------------------------------------
# Atom evaluation: True / False / None (unknown in this capture)
def ext_key(e):
    # Only the KHR and EXT spellings of one extension are aliases (promotions); other vendors' extensions with the
    # same suffix (VK_NV_mesh_shader vs VK_EXT_mesh_shader) are different extensions and keep their exact name.
    m = re.match(r"VK_(KHR|EXT)_(.+)", e)
    return m.group(2) if m else e


def has_ext(cap, e):
    k = ext_key(e)
    return True if any(ext_key(x) == k for x in cap["ext"]) else (False if cap["ext_complete"] else None)


def feature(cap, name):
    found = False
    for s in cap["feat"].values():
        if name in s:
            if s[name] is True:
                return True
            if s[name] is False:
                found = True
    if found:
        return False
    if name in core_feature_names():
        return None
    extensions = feature_extensions().get(name, set())
    if extensions and all(has_ext(cap, e) is False for e in extensions):
        return False
    return None


def feature_extensions():
    # Only explicit extension/feature pairs, not guesses from spelling. Core
    # promotions are not inferred: an absent probed core field remains unknown.
    if not hasattr(feature_extensions, "cache"):
        mapping = {"computeDerivativeGroupLinear": {"VK_KHR_compute_shader_derivatives", "VK_NV_compute_shader_derivatives"}}
        for entries in load_extracted()["dxvk"].values():
            for e in entries:
                if e["feature"] and e["ext"]:
                    mapping.setdefault(e["feature"], set()).add(e["ext"])
        feature_extensions.cache = mapping
    return feature_extensions.cache


def core_feature_names():
    if not hasattr(core_feature_names, "cache"):
        # The checked-in JSON defines which core fields this probe is expected
        # to export, including optional core features promoted from extensions.
        with open(os.path.join(DATA, "vi-v11.json")) as f:
            features = json.load(f)["capabilities"]["device"]["features"]
        core_feature_names.cache = {name for struct, values in features.items()
                                    if struct in ("VkPhysicalDeviceFeatures", "VkPhysicalDeviceVulkan11Features",
                                                  "VkPhysicalDeviceVulkan12Features", "VkPhysicalDeviceVulkan13Features")
                                    for name in values}
    return core_feature_names.cache


def expected_formats():
    if not hasattr(expected_formats, "cache"):
        # vulkaninfo omits unsupported formats; distinguish these from a
        # previously probed format that vanished from an incomplete capture.
        expected_formats.cache = set()
        for gpu in ("v10", "v11", "v13"):
            with open(os.path.join(DATA, "vi-%s.json" % gpu)) as f:
                expected_formats.cache.update(json.load(f)["capabilities"]["device"]["formats"])
    return expected_formats.cache


def prop_get(cap, path):
    parts = path.split(".")
    cur = cap["prop"].get(parts[0])
    for p in parts[1:]:
        if cur is None:
            return None
        cur = cur[int(p)] if isinstance(cur, list) and p.isdigit() and int(p) < len(cur) else (cur.get(p) if isinstance(cur, dict) else None)
    return cur


def fmt_bits(cap, fmt):
    p = cap["fmt"].get(fmt, {}).get("VkFormatProperties", {})
    return set(p.get("optimalTilingFeatures", [])), set(p.get("linearTilingFeatures", [])), set(p.get("bufferFeatures", []))


def atom(cap, a):
    if "|" in a:
        rs = [atom(cap, x) for x in a.split("|")]
        return True if True in rs else (None if None in rs else False)
    if a.startswith("F:"):
        return feature(cap, a[2:])
    if a.startswith("X:"):
        return has_ext(cap, a[2:])
    if a.startswith("API>="):
        maj, mi = map(int, a[5:].split("."))
        v = prop_get(cap, "VkPhysicalDeviceProperties.apiVersion")
        return None if not isinstance(v, int) else ((v >> 22) & 0x7f, (v >> 12) & 0x3ff) >= (maj, mi)
    if a == "Q:SPARSE":
        if not cap["queues"] or any("queueFlags" not in q for q in cap["queues"]):
            return None
        return any("VK_QUEUE_SPARSE_BINDING_BIT" in q["queueFlags"] for q in cap["queues"])
    if a.startswith("SG:"):
        spec = dict(x.split("=") for x in a[3:].split(";"))
        ops = prop_get(cap, "VkPhysicalDeviceVulkan11Properties.subgroupSupportedOperations")
        st = prop_get(cap, "VkPhysicalDeviceVulkan11Properties.subgroupSupportedStages")
        if ops is None or st is None:
            return None
        ops, st = set(ops), set(st)
        return set(spec["OPS"].split(",")) <= ops and set(spec["STAGES"].split(",")) <= st
    if a.startswith("FMT:") or a.startswith("BUF:"):
        if not cap["has_formats"]:
            return None
        _, f, bits = a.split(":")
        if f not in cap["fmt"] and f in expected_formats():
            return None
        if f in cap["fmt"]:
            p = cap["fmt"][f].get("VkFormatProperties", {})
            keys = ("bufferFeatures",) if a.startswith("BUF:") else ("optimalTilingFeatures", "linearTilingFeatures")
            if any(k not in p for k in keys):
                return None
        o, l, b = fmt_bits(cap, f)
        need = set(bits.split(","))
        return need <= b if a.startswith("BUF:") else (need <= o or need <= l)
    if a.startswith("P:"):
        m = re.match(r"P:([\w.]+)\s*(>=|<=|==|!=)\s*(\S+)", a)
        path, op, want = m.groups()
        v = prop_get(cap, path)
        if v is None:
            return None
        w = True if want == "True" else False if want == "False" else int(want) if re.match(r"^-?\d+$", want) else want
        if isinstance(w, bool) or isinstance(w, str):
            v = bool(v) if isinstance(w, bool) else v
        if isinstance(w, int) and not isinstance(w, bool) and not isinstance(v, (int, float)):
            return None
        return {">=": lambda: v >= w, "<=": lambda: v <= w, "==": lambda: v == w, "!=": lambda: v != w}[op]()
    raise ValueError("bad atom " + a)


def evaluate(cap, req):
    res = [(a, atom(cap, a)) for a in req["check"]]
    miss = [a for a, r in res if r is False]
    unk = [a for a, r in res if r is None]
    # A satisfied alternative does not prove that every expected field was
    # captured. Keep the row unresolved for the regression coverage gate.
    unk += [a for a, r in res if r is True and "|" in a and
            any(atom(cap, part) is None for part in a.split("|"))]
    return (False if miss else None if unk else True), miss, unk


# ---------------------------------------------------------------------------------------------------------------------
# Requirement list = curated + extracted DXVK table
def load_extracted():
    p = os.path.join(DATA, "extracted.json")
    if not os.path.exists(p):
        return {"dxvk": {}, "dxgi_formats": [], "panvk": {}}
    with open(p) as f:
        return json.load(f)


def requirements(ex=None):
    ex = ex or load_extracted()
    reqs = []
    for ver in DXVK_VERSIONS:
        for e in ex["dxvk"].get(ver, []):
            chk = []
            if e["ext"]:
                chk.append("X:" + e["ext"])
            if e["feature"]:
                chk.append("F:" + e["feature"])
            reqs.append(dict(id="%s-%s" % (ver, e["feature"] or e["ext"]), consumer=ver, level="device" if e["required"] else "optional",
                             hard=e["required"], check=chk, cite="%s/src/dxvk/dxvk_device_info.cpp:%d" % (ver, e["line"]),
                             effect="'Device does not support required feature' (no D3D8/9/10/11 device)" if e["required"]
                             else (e["purpose"] or "optional DXVK feature"), atomname=e["feature"] or e["ext"]))
        for c in CURATED:
            if c["consumer"] == "dxvk-*":
                reqs.append(dict(c, id="%s-%s" % (ver, c["id"][5:]), consumer=ver))
    reqs += [c for c in CURATED if c["consumer"] != "dxvk-*"]
    return reqs


LEVELS = {  # consumer -> ordered threshold levels
    **{v: ["device"] for v in DXVK_VERSIONS},
    "d3d11": ["11_0", "11_1", "12_0", "12_1"],
    "dxgi": ["format"],
    "vkd3d": ["device", "11_1", "12_0", "12_1", "12_2", "SM6.0", "SM6.2", "SM6.6"],
}


def report(cap, reqs):
    """-> {(consumer, level): (status, [ (req, miss, unk) ... failing ])}"""
    out = {}
    for r in reqs:
        st, miss, unk = evaluate(cap, r)
        k = (r["consumer"], r["level"])
        o = out.setdefault(k, [True, [], 0])
        o[2] += 1
        if st is not True:
            o[1].append((r, miss, unk))
            if st is False or (st is None and o[0] is True):
                o[0] = False if (st is False or o[0] is False) else None
    return out


def summary_lines(cap, reqs):
    rep = report(cap, reqs)
    lines = []
    for cons, levels in list(LEVELS.items()) + [(c, ["optional"]) for c in DXVK_VERSIONS + ["d3d11", "d3d9", "vkd3d"]]:
        for lv in levels:
            if (cons, lv) not in rep:
                continue
            st, fails, n = rep[(cons, lv)]
            tag = "PASS" if st is True else ("UNKNOWN" if st is None else "FAIL")
            hard = lv != "optional"
            lines.append((cons, lv, tag, n, [(r["id"], miss, unk) for r, miss, unk in fails], hard))
    return lines


def count_key(cons, lv):
    return "%s:%s" % (cons, lv)


def cmd_check(args):
    expect, caps = {}, []
    i = 0
    while i < len(args):
        if args[i] == "--expect":
            for kv in args[i + 1].split():
                k, v = kv.split("=")
                expect[k] = int(v)
            i += 2
        else:
            if args[i] != "-v":
                caps.append(args[i])
            i += 1
    reqs, rc = requirements(), 0
    for c in caps:
        cap = load_capture(c)
        print("== %s%s" % (c, " (partial capture: no formats)" if not cap["has_formats"] else ""))
        got = {}
        for cons, lv, tag, n, fails, hard in summary_lines(cap, reqs):
            got[count_key(cons, lv)] = (len(fails), sum(bool(u) for _, _, u in fails))
            if hard or tag != "PASS":
                print("%-12s %-9s %-7s %d/%d unresolved rows (%d known-failure, %d unknown)%s" %
                      (cons, lv, tag, len(fails), n, sum(bool(m) for _, m, _ in fails),
                       sum(bool(u) for _, _, u in fails),
                       (": " + ", ".join("%s[missing: %s; unknown: %s]" % (i_, " ".join(m) or "-", " ".join(u) or "-")
                                          for i_, m, u in fails)) if fails and (hard or "-v" in sys.argv) else ""))
        for k, v in expect.items():
            count, unknown = got.get(k, (None, None))
            if count != v or unknown != 0:
                print("FAIL  %s: %s unresolved rows, %s unknown rows; expected %d and zero unknown" % (k, count, unknown, v))
                rc = 1
    return rc


# ---------------------------------------------------------------------------------------------------------------------
# Extraction from the upstream clones
def extract_dxvk(root, core_h):
    src = os.path.join(root, "src/dxvk/dxvk_device_info.cpp")
    hdr = os.path.join(root, "src/dxvk/dxvk_device_info.h")
    macro = {}
    for l in open(hdr):
        m = re.search(r"VkExtensionProperties\s+(\w+)\s*=\s*vk::makeExtension\((\w+)\)", l)
        if m:
            macro[m.group(1)] = m.group(2)
    names = {}
    for h in core_h:
        if os.path.exists(h):
            for l in open(h):
                m = re.match(r'#define\s+(VK_\w+_EXTENSION_NAME)\s+"(\w+)"', l)
                if m:
                    names[m.group(1)] = m.group(2)
    out, purpose = [], ""
    lines = open(src).read().split("\n")
    inlist = False
    for n, l in enumerate(lines, 1):
        if "getFeatureList()" in l:
            inlist = True
        if not inlist:
            continue
        c = re.search(r"/\*\s*(.*?)\s*\*?/?$", l.strip())
        if l.strip().startswith("/*"):
            purpose = c.group(1).rstrip("*/ ") if c else ""
            j = n
            while not lines[j - 1].strip().endswith("*/") and j < len(lines):
                purpose += " " + lines[j].strip().rstrip("*/ ").lstrip("* ")
                j += 1
            continue
        if not l.strip():
            purpose = ""
        m = re.search(r"ENABLE_(FEATURE|EXT_FEATURE|EXT)\(([^)]*)\)", l)
        if not m or "#define" in l:
            continue
        kind, a = m.group(1), [x.strip() for x in m.group(2).split(",")]
        if kind == "FEATURE":
            ext, feat, req = None, a[1], a[2] == "true"
        elif kind == "EXT_FEATURE":
            ext, feat, req = a[0], a[1], a[2] == "true"
        else:
            ext, feat, req = a[0], None, a[1] == "true"
        en = None
        if ext:
            mac = macro.get(ext)
            en = names.get(mac) if mac else None
            if not en and mac:  # e.g. win32 headers: VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME -> best effort
                en = "VK_" + "_".join(mac.split("_")[1:-2])
        out.append(dict(line=n, ext=en, field=ext, feature=feat, required=req, purpose=purpose.strip()))
        if "}};" in l:
            break
    return out


def extract_dxgi(root):
    src = open(os.path.join(root, "src/dxgi/dxgi_format.cpp")).read().split("\n")
    out, i = [], 0
    while i < len(src):
        m = re.match(r"\s*// (DXGI_FORMAT_\w+)\s*$", src[i])
        if m and out is not None:
            body, j = "", i + 1
            while j < len(src) and "}" not in src[j]:
                body += src[j]; j += 1
            body += src[j] if j < len(src) else ""
            vks = re.findall(r"VK_FORMAT_\w+", body)
            out.append(dict(dxgi=m.group(1), line=i + 1, color=vks[0] if vks else None,
                            depth=vks[1] if len(vks) > 1 else None, raw=vks[2] if len(vks) > 2 else None))
            i = j
        if "g_dxgiFamilies" in src[i]:
            break
        i += 1
    return out


def extract_panvk(mesa):
    f = os.path.join(mesa, "src/panfrost/vulkan/panvk_vX_physical_device.c")
    res = {}
    for n, l in enumerate(open(f), 1):
        m = re.match(r"^\s+\.(\w+)\s*=\s*(.+?),?\s*(/\*.*)?$", l)
        if not m:
            continue
        key, expr = m.group(1), m.group(2).rstrip(",").strip()
        if re.match(r"^(KHR|EXT|ARM|VALVE|ANDROID|MESA|NV|AMD|GOOGLE)_", key):
            key = "VK_" + key
        vals = {}
        for arch in (10, 11, 13):
            e = expr.replace("PAN_ARCH", str(arch)).replace("&&", " and ").replace("||", " or ")
            e = re.sub(r"!(?!=)", " not ", e).replace("true", "True").replace("false", "False")
            # only constant boolean/integer expressions over PAN_ARCH are evaluated; anything else stays a condition
            if re.fullmatch(r"(\s|\d|[<>=!()]|True|False|and|or|not)+", e):
                try:
                    v = eval(e, {"__builtins__": {}}, {})
                except Exception:
                    v = None
            else:
                v = None
            vals[str(arch)] = v if isinstance(v, (bool, int)) else "cond:" + expr[:40]
        res.setdefault(key, dict(line=n, **vals))
    return res


def cmd_extract(args):
    refs = args[args.index("--refs") + 1] if "--refs" in args else "/tmp/opencode/feature-refs-dx"
    mesa = args[args.index("--mesa") + 1] if "--mesa" in args else os.path.join(HERE, "../../third_party/mesa")
    core_h = [os.path.join(mesa, "include/vulkan/vulkan_core.h"), os.path.join(mesa, "include/vulkan/vulkan_win32.h")]
    ex = dict(refs=REFS, dxvk={v: extract_dxvk(os.path.join(refs, v), core_h) for v in DXVK_VERSIONS},
              dxgi_formats=extract_dxgi(os.path.join(refs, "dxvk-master")), panvk=extract_panvk(mesa))
    os.makedirs(DATA, exist_ok=True)
    with open(os.path.join(DATA, "extracted.json"), "w") as f:
        json.dump(ex, f, indent=1, sort_keys=True)
    print("dxvk entries: %s; dxgi formats %d; panvk keys %d" % (
        {v: (len(e), sum(x["required"] for x in e)) for v, e in ex["dxvk"].items()}, len(ex["dxgi_formats"]), len(ex["panvk"])))
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# Markdown
def atoms_of(r):
    out = []
    for a in r["check"]:
        for x in a.split("|"):
            out.append(x)
    return out


def panvk_cell(ex, r):
    pv = ex.get("panvk", {})
    vals = []
    for arch in ("10", "11", "13"):
        ok, unknown = True, False
        for a in r["check"]:
            alt = []
            for x in a.split("|"):
                k = x[2:] if x[:2] in ("F:", "X:") else None
                if k is None or k not in pv:
                    alt.append(None)
                else:
                    v = pv[k][arch]
                    alt.append(v if isinstance(v, bool) else None)
            if True in alt:
                continue
            if None in alt:
                unknown = True
            else:
                ok = False
        vals.append("no" if not ok else "?" if unknown else "yes")
    return "/".join(vals) if len(set(vals)) > 1 else vals[0]


def cmd_markdown(args):
    caps = [load_capture(a) for a in args[:3]]
    ex, reqs = load_extracted(), requirements()
    P = print
    P("# Feature matrix: what Direct3D 8/9/10/11 (DXVK) and Direct3D 12 (vkd3d-proton) need from our Vulkan driver")
    P()
    P("Generated by `python3 tools/feature-matrix-dx/fmdx.py markdown tools/feature-matrix-dx/data/vi-v1{0,1,3}.json "
      "tools/feature-matrix-dx/data/phone/x4-1128-031240.txt tools/feature-matrix-dx/data/phone/cmf-1128-031509.txt` "
      "(the command `vk/check.sh` compares against) — do not edit by hand.")
    P("\"Ours\" comes only from `vulkaninfo --json` of this tree's ICD on the fake backend (`MALI_FAKE_GPU=v10|v11|v13`, captures in")
    P("`tools/feature-matrix-dx/data/`); \"PanVK\" is evaluated from `src/panfrost/vulkan/panvk_vX_physical_device.c` at the Mesa pin")
    P("(`?` = runtime condition or not a feature bit). Requirements cite path:line at these commits (clones in `/tmp/opencode/feature-refs-dx/`):")
    P()
    P("| tree | upstream | commit |")
    P("|---|---|---|")
    for k, (u, c) in REFS.items():
        P("| %s | %s | `%s` |" % (k, u, c))
    P()
    P("## 1. Thresholds today")
    P()
    P("| consumer | level | v10 | v11 | v13 | unresolved (v11: missing / unknown) |")
    P("|---|---|---|---|---|---|")
    sums = [summary_lines(c, reqs) for c in caps]
    for i, (cons, lv, tag, n, fails, hard) in enumerate(sums[1]):
        if not hard:
            continue
        tags = [s[i][2] + " (%d/%d; %d unknown)" % (len(s[i][4]), s[i][3], sum(bool(f[2]) for f in s[i][4])) if s[i][2] != "PASS" else "PASS" for s in sums]
        P("| %s | %s | %s | %s | %s | %s |" % (cons, lv, tags[0], tags[1], tags[2],
          ", ".join("`%s` (%s)" % (f[0], " / ".join(x for x, present in (("missing", bool(f[1])), ("unknown", bool(f[2]))) if present)) for f in fails) or "—"))
    P()
    P("Levels are cumulative: a D3D11 feature level needs the DXVK device plus every lower level; a vkd3d-proton feature level")
    P("needs `vkd3d/device`. `dxgi/format` rows have DXVK fallbacks (see the effect column). Counts are rows, not atoms;")
    P("known-failure and unknown rows can overlap. An expected threshold requires zero unknown atoms, even in a failing row.")
    P()
    P("## 2. Requirements")
    P()
    P("Hard = the consumer refuses (no device / lower feature level / lower shader model). Rows from DXVK's table are merged")
    P("across 2.7.1, 3.1.1 and master: `R` = required in that version, `o` = optional, `-` = not in the table.")
    P()
    groups = {}
    for r in reqs:
        if r["consumer"] in DXVK_VERSIONS:
            key = r.get("atomname") or r["id"]
            g = groups.setdefault(("dxvk", key), dict(r, vers={}))
            g["vers"][r["consumer"]] = ("R" if r["hard"] else "o", r["cite"].split(":")[-1])
            if r["hard"]:
                g["hard"], g["effect"], g["level"] = True, r["effect"], "device"
        else:
            groups[(r["consumer"], r["id"])] = r
    P("| requirement | needed by | hard | without it | PanVK v10/11/13 | ours v10 | ours v11 | ours v13 | hardware (Valhall) | source |")
    P("|---|---|---|---|---|---|---|---|---|---|")
    order = sorted(groups.items(), key=lambda kv: (0 if kv[0][0] == "dxvk" else 1, not kv[1]["hard"], kv[0][0], kv[1]["level"], kv[0][1]))
    for (cons, key), r in order:
        if cons == "dxvk":
            needed = "DXVK " + " / ".join("%s:%s" % (v[5:], r["vers"].get(v, ("-",))[0]) for v in DXVK_VERSIONS)
            cite = "dxvk_device_info.cpp " + ", ".join("%s:%s" % (v[5:], r["vers"][v][1]) for v in DXVK_VERSIONS if v in r["vers"])
            name = " ".join("`%s`" % a[2:] for a in r["check"])
        else:
            needed = "%s %s" % ({"d3d11": "D3D11 FL", "vkd3d": "vkd3d-proton", "d3d9": "D3D9", "dxgi": "DXGI/D3D9 formats"}.get(cons, cons), r["level"])
            cite = r["cite"]
            name = "`%s`: " % r["id"] + "; ".join(a.replace("|", " or ") for a in r["check"])
            if len(name) > 260:
                name = name[:257] + "..."
        ours = []
        for c in caps:
            st, miss, unk = evaluate(c, r)
            ours.append(("**no** + ?" if unk else "**no**") if st is False else "?" if st is None else "yes")
        hw = "; ".join(HW[a[2:]] for a in atoms_of(r) if a[2:] in HW)
        P("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (name, needed, "hard" if r["hard"] else "opt", r["effect"].replace("|", "/"),
          panvk_cell(ex, r), ours[0], ours[1], ours[2], hw or "", cite))
    P()
    P("## 3. DXGI formats (DXVK master `src/dxgi/dxgi_format.cpp`, extracted) against our optimal-tiling features")
    P()
    P("Columns are what DXVK can expose for that format given our bits (S sampled, F linear filter, R colour attachment, B blend,")
    P("U storage image, D depth/stencil attachment, V vertex buffer, T uniform texel buffer, W storage texel buffer). Which D3D11")
    P("format-support bits are *mandatory* per feature level comes from Microsoft's FL 11 format tables and is **not extracted**")
    P("here; the cited hard format rows (typed UAV loads, D24S8/D32S8) are in section 2.")
    P()
    P("| DXGI format | Vulkan (colour / depth) | v10 | v11 | v13 | src line |")
    P("|---|---|---|---|---|---|")
    bitmap = [("S", "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"), ("F", "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT"),
              ("R", "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT"), ("B", "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT"),
              ("U", "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT"), ("D", "VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT")]
    bufmap = [("V", "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT"), ("T", "VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT"),
              ("W", "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT")]
    for e in ex.get("dxgi_formats", []):
        if not e["color"] and not e["depth"]:
            continue
        cells = []
        for c in caps:
            s = ""
            for vf in (e["color"], e["depth"]):
                if not vf or vf == "VK_FORMAT_UNDEFINED":
                    continue
                o, l, b = fmt_bits(c, vf)
                s += "".join(ch for ch, bit in bitmap if bit in o) + "".join(ch for ch, bit in bufmap if bit in b) + "/"
            s = s.rstrip("/")
            cells.append(s or "**none**")
        vk = " / ".join(x[10:] for x in (e["color"], e["depth"]) if x and x != "VK_FORMAT_UNDEFINED")
        P("| %s | %s | %s | %s | %s | %d |" % (e["dxgi"][12:], vk, cells[0], cells[1], cells[2], e["line"]))
    P()
    P("D3D9 formats (`src/d3d9/d3d9_format.cpp`) are not extracted row by row; their fallbacks (D24S8 -> D32S8, D16S8) are rows in section 2.")
    if len(args) > 3:
        P()
        P("## 4. Partial cross-check against real-phone vulkaninfo text (no format section; extension inventory not assumed complete; driver layers loaded)")
        P()
        P("These are the only phone results in this document; sections 1-3 are fake-backend captures. UNKNOWN means the text")
        P("capture cannot decide the row (format rows, extensions the text inventory may omit), not that the item is missing.")
        P()
        P("| capture | " + " | ".join("%s:%s" % (c, l) for c, l, t, n, f, h in sums[1] if h) + " |")
        P("|---|" + "---|" * sum(1 for x in sums[1] if x[5]))
        for a in args[3:]:
            s = summary_lines(load_capture(a), reqs)
            P("| %s | " % "/".join(a.split("/")[-2:]) + " | ".join(
                ("PASS" if t == "PASS" else "%s %d (%d unknown)" % (t, len(f), sum(bool(x[2]) for x in f))) for c, l, t, n, f, h in s if h) + " |")
        P()
        P("Fake v11 for comparison: " + ", ".join("%s:%s=%d" % (c, l, len(f)) for c, l, t, n, f, h in sums[1] if h) + ".")
    return 0


def cmd_selftest(_):
    reqs = requirements()
    cap = load_capture(os.path.join(DATA, "vi-v11.json"))
    base = {count_key(c, l): len(f) for c, l, t, n, f, h in summary_lines(cap, reqs)}
    ok = True
    # 1. turning a satisfied DXVK hard feature off must add one missing row in every DXVK version
    cap["feat"]["VkPhysicalDeviceFeatures"]["dualSrcBlend"] = False
    got = {count_key(c, l): len(f) for c, l, t, n, f, h in summary_lines(cap, reqs)}
    for v in DXVK_VERSIONS:
        if got[v + ":device"] != base[v + ":device"] + 1:
            print("FAIL selftest: dualSrcBlend off did not move %s:device" % v); ok = False
    cap["feat"]["VkPhysicalDeviceFeatures"]["dualSrcBlend"] = True
    # 2. granting geometryShader must remove one (it is required by all three)
    cap["feat"]["VkPhysicalDeviceFeatures"]["geometryShader"] = True
    got = {count_key(c, l): len(f) for c, l, t, n, f, h in summary_lines(cap, reqs)}
    for v in DXVK_VERSIONS:
        if got[v + ":device"] != base[v + ":device"] - 1:
            print("FAIL selftest: geometryShader on did not move %s:device" % v); ok = False
    # 3. push descriptor extension must clear exactly the vkd3d push row
    cap["ext"].add("VK_KHR_push_descriptor")
    got = {count_key(c, l): len(f) for c, l, t, n, f, h in summary_lines(cap, reqs)}
    if got["vkd3d:device"] != base["vkd3d:device"] - 1:
        print("FAIL selftest: VK_KHR_push_descriptor did not move vkd3d:device"); ok = False
    # 3b. only KHR/EXT spellings alias: another vendor's extension with the same suffix does not satisfy the row
    al = dict(ext={"VK_NV_mesh_shader", "VK_KHR_line_rasterization"}, ext_complete=True)
    if has_ext(al, "VK_EXT_mesh_shader") is not False or has_ext(al, "VK_EXT_line_rasterization") is not True:
        print("FAIL selftest: extension alias rule"); ok = False
    # 4. the text parser reads the same features as the JSON for a rendered capture
    txt = "VkPhysicalDeviceFeatures:\n=========================\n\trobustBufferAccess = true\n\tgeometryShader = false\n" \
          "Device Extensions: count = 1\n\tVK_KHR_swapchain : extension revision 70\n"
    t = parse_text(txt, "inline")
    if not (atom(t, "F:robustBufferAccess") and not atom(t, "F:geometryShader") and atom(t, "X:VK_KHR_swapchain")):
        print("FAIL selftest: text parser"); ok = False
    if atom(t, "X:VK_EXT_missing") is not None or atom(t, "F:notProbed") is not None:
        print("FAIL selftest: partial text treated as complete"); ok = False
    # A known false field disappearing must not preserve a passing count gate.
    with open(os.path.join(DATA, "vi-v11.json")) as f:
        original = json.load(f)
    if feature(load_capture(os.path.join(DATA, "vi-v11.json")), "fragmentShaderPixelInterlock") is not False:
        print("FAIL selftest: absent extension-backed feature"); ok = False
    for label in ("false field", "capture subsection"):
        data = copy.deepcopy(original)
        dev = data["capabilities"]["device"]
        if label == "false field":
            del dev["features"]["VkPhysicalDeviceFeatures"]["geometryShader"]
            key = "dxvk-3.1.1:device"
        else:
            del dev["queueFamiliesProperties"]
            key = "vkd3d:12_0"  # already fails on other atoms in the same row
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as f:
            json.dump(data, f); f.flush()
            capture = load_capture(f.name)
            lines = summary_lines(capture, reqs)
            line = next(x for x in lines if count_key(x[0], x[1]) == key)
            with contextlib.redirect_stdout(io.StringIO()) as output:
                rc = cmd_check([f.name, "--expect", "%s=%d" % (key, base[key])])
            if len(line[4]) != base[key] or not any(u for _, _, u in line[4]) or rc != 1 or "unknown" not in output.getvalue():
                print("FAIL selftest: %s lost without failing unchanged-count gate" % label); ok = False
    if len(reqs) < 100:
        print("FAIL selftest: only %d requirements (data/extracted.json missing?)" % len(reqs)); ok = False
    print("selftest", "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    fn = {"check": cmd_check, "extract": cmd_extract, "markdown": cmd_markdown, "selftest": cmd_selftest}.get(cmd)
    if not fn:
        print(__doc__); sys.exit(2)
    sys.exit(fn(sys.argv[2:]))
