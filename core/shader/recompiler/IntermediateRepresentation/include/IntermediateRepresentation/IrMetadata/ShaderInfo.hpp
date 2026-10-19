#ifndef CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_IRMETADATA_SHADERINFO_HPP
#define CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_IRMETADATA_SHADERINFO_HPP

#include "IntermediateRepresentation/IrMetadata/Resources.hpp"
#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

struct ShaderInfo {
    std::uint32_t scratchDwords = 0;
    std::uint32_t sharedMemoryBytes = 0;
    // The descriptor-web split emits one arm-copy image per distinct descriptor
    // arm; a single source image can therefore fan out to many arm-copies, so the
    // cap must headroom beyond one arm per source (64 sources -> up to 64 copies).
    static constexpr std::uint32_t MaxImages = 128;
    static constexpr std::uint32_t MaxSamplers = 32;
    // Arm-copies fan out one sampled pair per descriptor arm, so this needs the
    // same headroom as MaxImages.
    static constexpr std::uint32_t MaxSampledPairs = 128;

    std::vector<BufferResource> buffers;
    std::vector<ImageResource> images;
    std::vector<std::vector<ImageResource>> runtimeImageModes;
    std::vector<SamplerResource> samplers;
    std::vector<SampledResourcePair> sampledPairs;
    std::vector<StageInput> inputs;
    std::vector<StageOutput> outputs;
    std::array<std::uint8_t, 32> vertexFetchComponents {};
    std::int32_t vertexOffsetSgpr = -1;
    std::int32_t instanceOffsetSgpr = -1;
    bool vertexOffsetShared = false;
    bool instanceOffsetShared = false;
    bool vertexOffsetConflict = false;
    bool instanceOffsetConflict = false;
    bool hasBitwiseXor = false;
    bool usesDma = false;
    bool bdaWrites = false;
    bool usesFaultBuffer = false;
    bool dispatchThreadLimit = false;
    bool rasterPrimitive = false;
    bool layerExported = false;

    bool operator==(const ShaderInfo& other) const = default;
};

[[nodiscard]] inline std::uint32_t WorkgroupMemoryStrideDwords(const ShaderInfo& info) {
    return info.sharedMemoryBytes == 0u ? 0u : info.sharedMemoryBytes / 4u + 1u;
}

}

#endif
