#include "Optimization/SrtWalker/SrtRuntimeValidator.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include "IntermediateRepresentation/IrBuilder.hpp"

namespace ShaderRecompiler::Detail {

bool RuntimeValidator::ValidateArguments(IrValue& inst, bool requireUniform) {
    for (std::size_t index = 0; index < inst.ArgumentCount(); index++) {
        if (!Validate(inst.Argument(index), requireUniform)) return false;
    }
    return true;
}

bool RuntimeValidator::Validate(IrValue* raw, bool requireUniform) {
    IrValue* value = raw->Resolve();
    if (_type == RuntimeValueType::Integer && TypesOverlap(value->Type(), IrType::F16 | IrType::F32 | IrType::Vec2F32)) {
        // #A132: F-typed nodes of a float->int bridge (image read result bitcast
        // to F32, then compared / balloted down to U32) are structural.
        if (value->Opcode() != IrOpcode::ImageRead && value->Opcode() != IrOpcode::BitCastF32U32) {
            if (getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
                std::fprintf(stderr, "[srt-slot-debug] fbridge reject id=%u op=%d args=%zu\n",
                            value->Id(), static_cast<int>(value->Opcode()), value->ArgumentCount());
            }
            return false;
        }
    }
    if (value->Opcode() == IrOpcode::Void) {
        if (!requireUniform) return true;
        switch (value->Type()) {
            case IrType::Bool:
            case IrType::U8:
            case IrType::U16:
            case IrType::U32:
            case IrType::U64:
            case IrType::F32: return true;
            default:
                if (getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
                    std::fprintf(stderr, "[srt-slot-debug] imm reject id=%u type=%d\n", value->Id(), static_cast<int>(value->Type()));
                }
                return false;
        }
    }
    // ReferenceU32 is the keep-alive wrapper SrtPlanBuilder::PatchReads emits over a
    // planningOnly SRT read. It is transparent: its value is its operand's, so validate the

    if (value->Opcode() == IrOpcode::ReferenceU32) {
        return value->ArgumentCount() == 1 && Validate(value->Argument(0), requireUniform);
    }
    IrValue& inst = *value;
    if (!requireUniform && _validatedDependencies.contains(&inst)) return true;
    if (!_visiting.insert(&inst).second) {
        if (requireUniform && getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
            std::fprintf(stderr, "[srt-slot-debug] cycle reject id=%u op=%d args=%zu\n",
                        inst.Id(), static_cast<int>(inst.Opcode()), inst.ArgumentCount());
        }
        return !requireUniform;
    }
    const auto finish = [&](bool valid) {
        _visiting.erase(&inst);
        if (valid && !requireUniform) _validatedDependencies.insert(&inst);
        if (!valid && getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
            std::fprintf(stderr, "[srt-slot-debug] finish-reject id=%u op=%d args=%zu uniform=%d\n",
                        inst.Id(), static_cast<int>(inst.Opcode()), inst.ArgumentCount(), requireUniform ? 1 : 0);
        }
        return valid;
    };
    const auto op = inst.Opcode();
    if (op == IrOpcode::ReadConst) {
        IrValue* slot = inst.ArgumentCount() == 2 ? inst.Argument(1)->Resolve() : nullptr;
        if (inst.ArgumentCount() != 2 || inst.Argument(0)->Resolve()->Opcode() == IrOpcode::Void || inst.Argument(0)->Resolve()->Opcode() != IrOpcode::GetSrtResource || slot == nullptr || slot->Type() != IrType::U32) {
            return finish(false);
        }
        if (slot->HasImmediate()) {
            if (slot->ImmediateU32() >= _program.srtReads.size()) {
                return finish(false);
            }
            if (_type == RuntimeValueType::Integer) {
                const auto activeMask = _activeMask;
                _activeMask = nullptr;
                const bool valid = Validate(_program.srtReads[slot->ImmediateU32()].value);
                _activeMask = activeMask;
                if (!valid) return finish(false);
            }
            // Immediate slot: the generic fall-through below validates the
            // GetSrtResource + constant exactly as before.
        } else {

            // constant graph over slot indices. Every selectable constant slot
            // must be in range and (for Integer) hold a valid descriptor value.
            // Short-circuit here: the uniform fall-through would reject the
            // non-invariant slot phi.
            std::unordered_set<IrValue*> slotVisiting;
            const bool slotOk = ValidateSrtSlot(slot, slotVisiting, _type == RuntimeValueType::Integer);
            if (getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
                std::fprintf(stderr, "[srt-slot-debug] runtime-slot ReadConst: slotValid=%d type=%d args=%zu\n",
                            static_cast<int>(slotOk), static_cast<int>(_type), inst.ArgumentCount());
            }
            if (!slotOk) {
                return finish(false);
            }
            return finish(true);
        }
    }
    if (!requireUniform) return finish(ValidateArguments(inst, false));
    if (IsRuntimeSelect(op) && inst.ArgumentCount() == 3) {
        // The condition is a control-flow value (a lane mask, or the web path's
        // EdgeSelector branch predicate) — not a runtime resource — so validate
        // the arms only and leave the condition to the surrounding control flow.
        if (_type == RuntimeValueType::Integer && !Validate(inst.Argument(2), false)) {
            return finish(false);
        }
        return finish(Validate(inst.Argument(1)));
    }
    if (op == IrOpcode::UndefU1 || op == IrOpcode::UndefU8 || op == IrOpcode::UndefU16 || op == IrOpcode::UndefU32 || op == IrOpcode::UndefU64 || op == IrOpcode::Void) {
        return finish(false);
    }
    if (op == IrOpcode::GetUserData) {
        if (inst.ArgumentCount() != 1 || inst.Argument(0)->Type() != IrType::ScalarReg) {
            return finish(false);
        }
        const auto reg = RegIndex(static_cast<ScalarReg>(inst.Argument(0)->Register().index));
        if (reg < _program.userDataBase || reg - _program.userDataBase >= _program.userDataCount) {
            return finish(false);
        }
        return finish(true);
    }
    if (op == IrOpcode::GetShaderBase) {
        if (inst.ArgumentCount() != 0) {
            return finish(false);
        }
        return finish(true);
    }
    if (op == IrOpcode::Phi) {
        if (_type == RuntimeValueType::Integer && !ValidateArguments(inst, false)) {
            return finish(false);
        }
        IrValue* invariant = ResolveInvariantPhi(_program, &inst);
        if (invariant == nullptr) {
            // #A134: non-invariant CFG-Phi whose arms are all valid runtime values —

            // bit-clear loop carries x -> x & (x-1) across the backedge; the
            // descriptor dwords are indexed by ctz of that value). At runtime the
            // hardware holds whichever arm's value the executed path produced, so
            // the phi is a runtime value exactly when its arms are. Mirrors the
            // #A131 runtime-slot acceptance (non-invariant phis over valid leaves).
            if (ValidateArguments(inst, false)) {
                if (getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
                    std::fprintf(stderr, "[srt-slot-debug] Phi accepted: non-invariant runtime arms, args=%zu\n", inst.ArgumentCount());
                }
                return finish(true);
            }
            if (getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
                std::fprintf(stderr, "[srt-slot-debug] Phi rejected: no invariant resolution, args=%zu\n", inst.ArgumentCount());
            }
            return finish(false);
        }
        return finish(Validate(invariant));
    }
    if (op == IrOpcode::ReadFirstLane) {
        if (inst.ArgumentCount() != 2 || inst.Argument(0)->Type() != IrType::U32 || inst.Argument(1)->Type() != IrType::Bool) {
            return finish(false);
        }
        if (_type == RuntimeValueType::Integer && !Validate(inst.Argument(1), false)) {
            return finish(false);
        }
        const auto activeMask = _activeMask;
        _activeMask = inst.Argument(1)->Resolve();
        const bool valid = Validate(inst.Argument(0));
        _activeMask = activeMask;
        return finish(valid);
    }
    if (op == IrOpcode::GetSrtResource) {
        if (inst.ArgumentCount() != 0) {
            return finish(false);
        }
        return finish(true);
    }
    if (op == IrOpcode::LoadAddressU32 || op == IrOpcode::ReadConstBuffer) {
        const auto expected = op == IrOpcode::LoadAddressU32 ? IrOpcode::GetAddressResource : IrOpcode::GetBufferResource;
        IrValue* handle = inst.ArgumentCount() != 0 ? inst.Argument(0)->Resolve() : nullptr;
        if (!IsRawRead(_program, inst) || handle == nullptr || handle->Opcode() != expected) {
            return finish(false);
        }
    } else if (op == IrOpcode::CompositeExtractU64) {
        IrValue* index = inst.ArgumentCount() == 2 ? inst.Argument(1)->Resolve() : nullptr;
        if (index == nullptr || !index->HasImmediate() || index->Type() != IrType::U32 || index->ImmediateU32() >= 2u) {
            return finish(false);
        }
    } else if (op == IrOpcode::CompositeExtractU32x2) {
        IrValue* source = inst.ArgumentCount() == 2 ? inst.Argument(0)->Resolve() : nullptr;
        IrValue* index = inst.ArgumentCount() == 2 ? inst.Argument(1)->Resolve() : nullptr;
        if (source == nullptr || index == nullptr || !index->HasImmediate() || index->Type() != IrType::U32 || index->ImmediateU32() >= 2u || (source->Opcode() != IrOpcode::CompositeConstructU32x2 && source->Opcode() != IrOpcode::IAddCarry32)) {
            return finish(false);
        }
    } else if (op == IrOpcode::Ballot) {
        // #A132: Ballot collapses per-lane divergence into a wave-uniform mask —
        // uniform by hardware semantics regardless of its (per-lane) argument.
        // The argument is deliberately NOT descended in uniform mode: per-lane

        // 1160925800 dword 0 = ballot of a per-lane float sign test).
        if (requireUniform) {
            return finish(true);
        }
    } else if (op == IrOpcode::ImageRead) {
        // #A132: runtime image read at a fully-immediate address — every lane reads
        // the same texel, so the result is wave-uniform and valid as a runtime

        // test on the sampled value).
        if (inst.ArgumentCount() != 3u || !inst.Argument(2)->HasImmediate()) {
            return finish(false);
        }
        IrValue* address = inst.Argument(1)->Resolve();
        if (address->Opcode() != IrOpcode::MakeImageAddress) {
            return finish(false);
        }
        for (std::size_t i = 0; i < address->ArgumentCount(); i++) {
            if (!address->Argument(i)->HasImmediate()) {
                return finish(false);
            }
        }
    } else if (op == IrOpcode::MakeImageAddress) {
        for (std::size_t i = 0; i < inst.ArgumentCount(); i++) {
            if (!inst.Argument(i)->HasImmediate()) {
                return finish(false);
            }
        }
    }
    if (op == IrOpcode::ReadLane || op == IrOpcode::DppMoveU32 || op == IrOpcode::DppUpdateU32 ||
        op == IrOpcode::Permlane16U32 || op == IrOpcode::LaneId || op == IrOpcode::GetBuiltin ||
        op == IrOpcode::ImageSampleRaw) {

        // dwords are computed from per-lane data — ReadLane lane picks, DPP
        // cross-lane updates, Permlane16 shuffles, LaneId, GetBuiltin dispatch
        // data, and raw image samples feeding the bit-clear/ctz descriptor chain).
        // The hardware evaluates these from other runtime values at runtime, so
        // validate the data arguments non-uniformly: per-lane values (including
        // non-invariant loop-carried phis via the Phi fallback above) are legal
        // operands — the same acceptance #A131 gives runtime SRT slots.
        return finish(ValidateArguments(inst, false));
    }
    if (IsDescriptorHandle(op)) {
        std::size_t expected = 4u;
        if (op == IrOpcode::GetImageResource) {
            expected = 8u;
        } else if (op == IrOpcode::GetAddressResource) {
            expected = 2u;
        }
        if (inst.ArgumentCount() != expected) {
            return finish(false);
        }
    } else if (op != IrOpcode::ReadConst && op != IrOpcode::ReadConstBuffer && op != IrOpcode::LoadAddressU32 && op != IrOpcode::ImageRead && op != IrOpcode::MakeImageAddress && !IsRuntimeUniformOp(op)) {
        if (getenv("APS5_SRT_SLOT_DEBUG") != nullptr) {
            std::fprintf(stderr, "[srt-slot-debug] gate rejected: op=%d args=%zu type=%d uniform=%d\n",
                        static_cast<int>(op), inst.ArgumentCount(), static_cast<int>(_type), requireUniform ? 1 : 0);
        }
        return finish(false);
    }
    return finish(ValidateArguments(inst, true));
}


// a CFG-Phi / constant graph over SRT slot indices. Every constant slot must
// be in range; when checkDescriptors, each slot's descriptor value must also
// be a valid runtime value (mirrors the immediate-slot check above). Cycles
// (loop-carried slot phis, verified live in #A132) are accepted: only phis
// ever enter the visiting set and every leaf was range-checked at entry.
bool RuntimeValidator::ValidateSrtSlot(IrValue* value, std::unordered_set<IrValue*>& visiting, bool checkDescriptors) {
    value = value->Resolve();
    if (value->HasImmediate()) {
        if (value->Type() != IrType::U32 || value->ImmediateU32() >= _program.srtReads.size()) {
            return false;
        }
        if (checkDescriptors) {
            const auto activeMask = _activeMask;
            _activeMask = nullptr;
            const bool valid = Validate(_program.srtReads[value->ImmediateU32()].value);
            _activeMask = activeMask;
            if (!valid) return false;
        }
        return true;
    }
    if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() == 2u) {
        // Leaf: ReadConst(GetSrtResource, immediate slot) — the ladder's terminal read.
        IrValue* resource = value->Argument(0)->Resolve();
        IrValue* slot = value->Argument(1)->Resolve();
        if (resource->Opcode() != IrOpcode::GetSrtResource || slot == nullptr || !slot->HasImmediate()
            || slot->Type() != IrType::U32 || slot->ImmediateU32() >= _program.srtReads.size()) {
            return false;
        }
        if (checkDescriptors) {
            const auto activeMask = _activeMask;
            _activeMask = nullptr;
            const bool valid = Validate(_program.srtReads[slot->ImmediateU32()].value);
            _activeMask = activeMask;
            if (!valid) return false;
        }
        return true;
    }
    if (!value->IsPhi()) {
        return false;
    }
    if (!visiting.insert(value).second) {
        return true;  // cycle through phis (loop-carried slot): structurally valid, leaves range-checked at entry
    }
    for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
        if (!ValidateSrtSlot(value->Argument(index), visiting, checkDescriptors)) {
            visiting.erase(value);
            return false;
        }
    }
    visiting.erase(value);
    return true;
}

}
