#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCETRACKER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCETRACKER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

namespace ShaderRecompiler {

class ResourceTracker {
public:
    void Track(IrProgram& program) const;
};

// a254 SRT-port (#A260-Doc): fold per-lane SRT value-ladder phis into ReadConst(SRT, slot-phi) so
// the SrtEvaluator resolves them (re-adds the a240 FoldLadderSlotReads dropped by the upstream

void FoldImageResourceSlotLadders(IrProgram& program);

}

#endif
