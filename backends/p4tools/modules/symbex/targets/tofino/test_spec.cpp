/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include "backends/p4tools/modules/symbex/targets/tofino/test_spec.h"

#include <random>

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>

#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/variables.h"
#include "ir/irutils.h"

namespace P4::P4Tools::Symbex::Tofino {

using namespace P4::literals;

/* =========================================================================================
 *  IndexExpression
 * ========================================================================================= */

IndexExpression::IndexExpression(const IR::Expression *index, const IR::Expression *value)
    : index(index), value(value) {}

const IR::Constant *IndexExpression::getEvaluatedValue() const {
    const auto *constant = value->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return constant;
}

const IR::Constant *IndexExpression::getEvaluatedIndex() const {
    const auto *constant = index->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return constant;
}

const IR::Expression *IndexExpression::getIndex() const { return index; }

const IR::Expression *IndexExpression::getValue() const { return value; }

cstring IndexExpression::getObjectName() const { return "IndexExpression"_cs; }

const IndexExpression *IndexExpression::evaluate(const Model &model, bool doComplete) const {
    const auto *evaluatedIndex = model.evaluate(index, doComplete);
    const auto *evaluatedValue = model.evaluate(value, doComplete);
    return new IndexExpression(evaluatedIndex, evaluatedValue);
}

std::map<big_int, std::pair<int, const IR::Constant *>> IndexMap::unravelMap() const {
    std::map<big_int, std::pair<int, const IR::Constant *>> valueMap;
    for (auto it = indexConditions.rbegin(); it != indexConditions.rend(); ++it) {
        const auto *storedIndex = it->getEvaluatedIndex();
        const auto *storedVal = it->getEvaluatedValue();
        // Important, if the element already exists in the map, ignore it.
        // That index has been overwritten.
        valueMap.insert({storedIndex->value, {storedIndex->type->width_bits(), storedVal}});
    }
    return valueMap;
}

/* =========================================================================================
 *  IndexMap
 * ========================================================================================= */

IndexMap::IndexMap(const IR::Expression *initialValue) : initialValue(initialValue) {}

void IndexMap::writeToIndex(const IR::Expression *index, const IR::Expression *value) {
    indexConditions.emplace_back(index, value);
}

const IR::Expression *IndexMap::getInitialValue() const { return initialValue; }

std::optional<big_int> IndexMap::getCarriedScalarValue() const {
    if (const auto *constant = initialValue->to<IR::Constant>()) {
        return constant->value;
    }
    return std::nullopt;
}

const IR::Expression *IndexMap::getValueAtIndex(const IR::Expression *index) const {
    const IR::Expression *baseExpr = initialValue;
    for (const auto &indexMap : indexConditions) {
        const auto *storedIndex = indexMap.getIndex();
        const auto *storedVal = indexMap.getValue();
        baseExpr =
            new IR::Mux(baseExpr->type, new IR::Equ(storedIndex, index), storedVal, baseExpr);
    }
    return baseExpr;
}

const IR::Expression *IndexMap::getEvaluatedInitialValue() const {
    if (const auto *constant = initialValue->to<IR::Constant>()) {
        return constant;
    }
    if (const auto *initalListExprValue = initialValue->to<IR::StructExpression>()) {
        // TODO: Validate that this is a list expression of constants.
        return initalListExprValue;
    }
    BUG("Variable is not a constant or a list expression, has the test object %1% been evaluated?",
        getObjectName());
}

/* =========================================================================================
 *  TofinoRegisterValue
 * ========================================================================================= */

TofinoRegisterValue::TofinoRegisterValue(const IR::Declaration_Instance *decl,
                                         const IR::Expression *initialValue,
                                         const IR::Expression *initialIndex)
    : IndexMap(initialValue), decl(decl), initialIndex(initialIndex) {}

cstring TofinoRegisterValue::getObjectName() const { return "TofinoRegisterValue"_cs; }

const IR::Declaration_Instance *TofinoRegisterValue::getRegisterDeclaration() const { return decl; }

const TofinoRegisterValue *TofinoRegisterValue::evaluate(const Model &model,
                                                         bool doComplete) const {
    const IR::Expression *evaluatedValue = nullptr;
    if (const auto *initalListExprValue = initialValue->to<IR::StructExpression>()) {
        evaluatedValue = model.evaluateStructExpr(initalListExprValue, doComplete);
    } else {
        evaluatedValue = model.evaluate(initialValue, doComplete);
    }
    const auto *evaluatedInitialIndex = model.evaluate(initialIndex, doComplete);
    return new TofinoRegisterValue(decl, evaluatedValue, evaluatedInitialIndex);
}

const TestObject *TofinoRegisterValue::evaluateForCarry(const Model &model) const {
    // Determine this register's contents at initialIndex after all recorded writes
    // (last-write-wins), then evaluate that single value to a concrete expression and use it
    // as the snapshot's initialValue with NO indexConditions. This avoids the per-index Mux
    // that getValueAtIndex() would build (which model.evaluate() cannot collapse for struct
    // registers and which initializeRegisterParameters() cannot consume), while still
    // carrying the post-write value a later phase must read.
    const IR::Expression *finalValue = initialValue;
    const auto *evalIndex = model.evaluate(initialIndex, /*doComplete=*/true);
    for (const auto &cond : indexConditions) {
        const auto *condIndex = model.evaluate(cond.getIndex(), /*doComplete=*/true);
        if (condIndex->equiv(*evalIndex)) {
            finalValue = cond.getValue();  // later writes overwrite earlier ones
        }
    }
    const IR::Expression *evaluatedValue = nullptr;
    if (const auto *structVal = finalValue->to<IR::StructExpression>()) {
        evaluatedValue = model.evaluateStructExpr(structVal, /*doComplete=*/true);
    } else {
        evaluatedValue = model.evaluate(finalValue, /*doComplete=*/true);
    }
    return new TofinoRegisterValue(decl, evaluatedValue, evalIndex);
}

const TestObject *TofinoRegisterValue::withCarriedScalarValue(big_int value) const {
    // initialValue is the post-carry folded constant; replace it with a constant of the same width.
    const auto *valueType = initialValue->type;
    return new TofinoRegisterValue(decl, IR::Constant::get(valueType, value), initialIndex);
}

bool TofinoRegisterValue::hasTaintedIndex() const {
    // The read/initial index (set at RegisterAction.execute) and any recorded write indices. A
    // RANDOM-hash-indexed register carries a tainted index here (symbex can't resolve the hash).
    if (initialIndex != nullptr && Taint::hasTaint(initialIndex)) return true;
    for (const auto &cond : indexConditions) {
        if (Taint::hasTaint(cond.getIndex())) return true;
    }
    return false;
}

std::vector<const IR::Expression *> TofinoRegisterValue::getIndexExpressions() const {
    // POSITIONAL CONTRACT: slot 0 is always the read/initial index and every later slot is a
    // recorded WRITE index, so a caller can tell the two apart by position alone. Slot 0 is
    // therefore emitted even when the register has no read index, as a nullptr placeholder --
    // dropping it would silently promote the first write into the read slot, and a caller asking
    // "which cells does this packet write?" would then miss that write. Every caller already skips
    // nullptr entries.
    std::vector<const IR::Expression *> indices;
    indices.push_back(initialIndex);
    for (const auto &cond : indexConditions) {
        indices.push_back(cond.getIndex());
    }
    return indices;
}

const IR::Expression *declaredRegisterInitialValue(const IR::Declaration_Instance *decl) {
    if (decl == nullptr || decl->arguments == nullptr) {
        return nullptr;
    }
    // Resolve the extern's base name through the specialization wrapper: the declaration reads
    // `Register<key_pair_t, index_t>(CACHE_SIZE, {1, 0}) keys;`, so decl->type is a specialized
    // type whose base carries the name.
    cstring externName;
    if (const auto *spec = decl->type->to<IR::Type_Specialized>();
        spec != nullptr && spec->baseType != nullptr && spec->baseType->path != nullptr) {
        externName = spec->baseType->path->name.name;
    } else if (const auto *named = decl->type->to<IR::Type_Name>();
               named != nullptr && named->path != nullptr) {
        externName = named->path->name.name;
    } else {
        return nullptr;
    }
    // Register(size) has no init; Register(size, init) puts it second; DirectRegister(init) first.
    size_t initIdx = 0;
    if (externName == "Register") {
        if (decl->arguments->size() < 2) {
            return nullptr;
        }
        initIdx = 1;
    } else if (externName == "DirectRegister") {
        if (decl->arguments->empty()) {
            return nullptr;
        }
    } else {
        return nullptr;
    }
    return decl->arguments->at(initIdx)->expression;
}

std::optional<big_int> TofinoRegisterValue::registerCellCount() const {
    // Register<T, I>(bit<32> size [, T init]) — the first ctor arg is the cell count.
    if (decl == nullptr || decl->arguments == nullptr || decl->arguments->empty()) {
        return std::nullopt;
    }
    const auto *sizeExpr = decl->arguments->at(0)->expression;
    if (const auto *sizeConst = sizeExpr->to<IR::Constant>()) {
        return sizeConst->value;
    }
    return std::nullopt;
}

const IR::Constant *TofinoRegisterValue::maskIndex(const IR::Constant *idx) const {
    if (idx == nullptr) return idx;
    auto cells = registerCellCount();
    if (!cells.has_value() || *cells <= 0) return idx;
    big_int n = *cells;
    // Only power-of-two sizes fold cleanly to low-bit addressing; leave other sizes raw (sound).
    if ((n & (n - 1)) != 0) return idx;
    if (idx->value >= 0 && idx->value < n) return idx;  // already in range
    return IR::Constant::get(idx->type, idx->value % n);
}

const IR::Constant *TofinoRegisterValue::getEvaluatedInitialIndex() const {
    const auto *constant = initialIndex->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return maskIndex(constant);
}

AttackerControlResult TofinoRegisterValue::withAttackerValues(
    const Model &model, std::optional<big_int> fixedValue,
    const std::vector<big_int> &forbiddenValues) const {
    auto *randReg = new TofinoRegisterValue(decl, getInitialValue(), initialIndex);
    std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>> modelOverrides;
    bool feasible = true;
    auto isForbidden = [&](const big_int &v) {
        return std::find(forbiddenValues.begin(), forbiddenValues.end(), v) !=
               forbiddenValues.end();
    };
    for (const auto &cond : indexConditions) {
        const auto *concreteIdx =
            model.evaluate(cond.getIndex(), /*doComplete=*/true)->checkedTo<IR::Constant>();
        const auto *symVal = cond.getValue();
        const auto *concreteVal =
            model.evaluate(symVal, /*doComplete=*/true)->checkedTo<IR::Constant>();
        const auto *symVar = symVal->to<IR::SymbolicVariable>();
        const IR::Constant *attackerVal = nullptr;
        if (symVar != nullptr) {
            // Packet-controllable write: the attacker chooses the value. Use the requested fixed
            // value, else a random one avoiding the forbidden (Phase-1 HIT-key) set, and inject
            // it into the Phase-2 packet via a model override.
            attackerVal = [&]() -> const IR::Constant * {
                if (fixedValue.has_value()) {
                    const auto *bitsType = concreteVal->type->to<IR::Type_Bits>();
                    if (bitsType == nullptr) {
                        return IR::Constant::get(concreteVal->type, *fixedValue);
                    }
                    // The requested value is width-limited by the register cell: 0xdeadbeef on a
                    // bit<1> lock truncates to 1. Test the TRUNCATED value against the Phase-1
                    // HIT-key set (`forbiddenValues`) — a raw-value test wrongly passes here and
                    // emits a non-flipping tamper (e.g. re-writing the victim's own `acquire`=1).
                    big_int maxVal = IR::getMaxBvVal(bitsType->width_bits());
                    big_int truncated = *fixedValue & maxVal;
                    if (!isForbidden(truncated)) {
                        return IR::Constant::get(bitsType, truncated);
                    }
                    // The truncated request collides with a HIT-key value (HIT preserved in
                    // Phase 3 -> no sink flip). Override it with a flipping value: only
                    // |forbiddenValues| values are forbidden, so [0 .. |forbidden|] holds one.
                    for (big_int cand = 0; cand <= maxVal && cand <= big_int(forbiddenValues.size());
                         ++cand) {
                        if (!isForbidden(cand)) {
                            ::P4::warning(
                                "[Tampering] --state-tamper-value 0x%1% truncates to a Phase-1 "
                                "table-key value at width %2%; using 0x%3% instead so the sink "
                                "actually flips.",
                                fixedValue->str(0, std::ios_base::hex), bitsType->width_bits(),
                                cand.str(0, std::ios_base::hex));
                            return IR::Constant::get(bitsType, cand);
                        }
                    }
                    // Register too narrow to avoid the HIT set — keep the request; the
                    // differential oracle is the final judge.
                    return IR::Constant::get(bitsType, truncated);
                }
                const auto *bitsType = concreteVal->type->to<IR::Type_Bits>();
                big_int maxVal = IR::getMaxBvVal(bitsType->width_bits());
                boost::random::mt19937 localRng(std::random_device{}());
                boost::random::uniform_int_distribution<big_int> dist(0, maxVal);
                for (int attempt = 0; attempt < 64; ++attempt) {
                    big_int candidate = dist(localRng);
                    if (!isForbidden(candidate)) {
                        return IR::Constant::get(bitsType, candidate);
                    }
                }
                return IR::Constant::get(bitsType, dist(localRng));
            }();
            modelOverrides.emplace_back(symVar, attackerVal);
        } else {
            // Program-fixed write (e.g. `value = LOCK_SHARED`), not packet-derived: the attacker
            // cannot choose it, so --state-tamper-value does not apply. The emitted value must be
            // exactly what the packet writes (concreteVal). The tamper only flips the sink if that
            // value avoids the forbidden (HIT-key) set; otherwise this Phase-2 packet is no tamper.
            attackerVal = concreteVal;
            if (isForbidden(concreteVal->value)) {
                feasible = false;
            }
        }
        // Fold the emitted cell index into the register's real address space (the symbolic run
        // used the raw CRC/hash index; the harness must pre-set/read the cell the hardware accesses).
        randReg->writeToIndex(maskIndex(concreteIdx), attackerVal);
    }
    return {randReg, modelOverrides, feasible};
}

/* =========================================================================================
 *  TofinoDirectRegisterValue
 * ========================================================================================= */

TofinoDirectRegisterValue::TofinoDirectRegisterValue(const IR::Declaration_Instance *decl,
                                                     const IR::Expression *initialValue,
                                                     const IR::P4Table *table)
    : initialValue(initialValue), decl(decl), table(table) {}

cstring TofinoDirectRegisterValue::getObjectName() const { return "TofinoDirectRegisterValue"_cs; }

const IR::Declaration_Instance *TofinoDirectRegisterValue::getRegisterDeclaration() const {
    return decl;
}

const IR::P4Table *TofinoDirectRegisterValue::getRegisterTable() const { return table; }

const TofinoDirectRegisterValue *TofinoDirectRegisterValue::evaluate(const Model &model,
                                                                     bool doComplete) const {
    const IR::Expression *evaluatedValue = nullptr;
    if (const auto *initalListExprValue = initialValue->to<IR::StructExpression>()) {
        evaluatedValue = model.evaluateStructExpr(initalListExprValue, doComplete);
    } else {
        evaluatedValue = model.evaluate(initialValue, doComplete);
    }
    return new TofinoDirectRegisterValue(decl, evaluatedValue, table);
}

const IR::Expression *TofinoDirectRegisterValue::getInitialValue() const { return initialValue; }

const IR::Expression *TofinoDirectRegisterValue::getEvaluatedInitialValue() const {
    if (const auto *constant = initialValue->to<IR::Constant>()) {
        return constant;
    }
    if (const auto *initalListExprValue = initialValue->to<IR::StructExpression>()) {
        // TODO: Validate that this is a list expression of constants.
        return initalListExprValue;
    }
    BUG("Variable is not a constant or a list expression, has the test object %1% been evaluated?",
        getObjectName());
}

/* =========================================================================================
 *  TofinoRegisterParam
 * ========================================================================================= */

TofinoRegisterParam::TofinoRegisterParam(const IR::Declaration_Instance *decl,
                                         const IR::Expression *initialValue)
    : decl(decl), initialValue(initialValue) {}

cstring TofinoRegisterParam::getObjectName() const { return "TofinoRegisterParam"_cs; }

const TofinoRegisterParam *TofinoRegisterParam::evaluate(const Model &model,
                                                         bool doComplete) const {
    return new TofinoRegisterParam(decl, model.evaluate(initialValue, doComplete));
}

const IR::Declaration_Instance *TofinoRegisterParam::getRegisterParamDeclaration() const {
    return decl;
}

const IR::Expression *TofinoRegisterParam::getInitialValue() const { return initialValue; }

const IR::Constant *TofinoRegisterParam::getEvaluatedInitialValue() const {
    const auto *constant = initialValue->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return constant;
}

/* =========================================================================================
 *  TofinoActionProfile
 * ========================================================================================= */

const std::vector<std::pair<cstring, std::vector<ActionArg>>> *TofinoActionProfile::getActions()
    const {
    return &actions;
}

TofinoActionProfile::TofinoActionProfile(const IR::IDeclaration *profileDecl)
    : profileDecl(profileDecl) {}

cstring TofinoActionProfile::getObjectName() const { return profileDecl->controlPlaneName(); }

const IR::IDeclaration *TofinoActionProfile::getProfileDecl() const { return profileDecl; }

void TofinoActionProfile::addToActionMap(cstring actionName, std::vector<ActionArg> actionArgs) {
    actions.emplace_back(actionName, actionArgs);
}
size_t TofinoActionProfile::getActionMapSize() const { return actions.size(); }

const TofinoActionProfile *TofinoActionProfile::evaluate(const Model &model,
                                                         bool doComplete) const {
    auto *profile = new TofinoActionProfile(profileDecl);
    for (const auto &actionTuple : actions) {
        auto actionArgs = actionTuple.second;
        std::vector<ActionArg> evaluatedArgs;
        evaluatedArgs.reserve(actionArgs.size());
        for (const auto &actionArg : actionArgs) {
            evaluatedArgs.emplace_back(*actionArg.evaluate(model, doComplete));
        }
        profile->addToActionMap(actionTuple.first, evaluatedArgs);
    }
    return profile;
}

/* =========================================================================================
 *  TofinoActionSelector
 * ========================================================================================= */

cstring TofinoActionSelector::getObjectName() const { return "TofinoActionSelector"_cs; }

const IR::IDeclaration *TofinoActionSelector::getSelectorDecl() const { return selectorDecl; }

const TofinoActionProfile *TofinoActionSelector::getActionProfile() const { return actionProfile; }

TofinoActionSelector::TofinoActionSelector(const IR::IDeclaration *selectorDecl,
                                           const TofinoActionProfile *actionProfile)
    : selectorDecl(selectorDecl), actionProfile(actionProfile) {}

const TofinoActionSelector *TofinoActionSelector::evaluate(const Model &model,
                                                           bool doComplete) const {
    const auto *evaluatedProfile = actionProfile->evaluate(model, doComplete);
    return new TofinoActionSelector(selectorDecl, evaluatedProfile);
}

/* =========================================================================================
 * Table Key Match Types
 * ========================================================================================= */

Range::Range(const IR::KeyElement *key, const IR::Expression *low, const IR::Expression *high)
    : TableMatch(key), low(low), high(high) {}

const IR::Constant *Range::getEvaluatedLow() const {
    const auto *constant = low->to<IR::Constant>();
    BUG_CHECK(constant,
              "Variable is not a constant. It has type %1% instead. Has the test object %2% "
              "been evaluated?",
              low->type->node_type_name(), getObjectName());
    return constant;
}

const IR::Constant *Range::getEvaluatedHigh() const {
    const auto *constant = high->to<IR::Constant>();
    BUG_CHECK(constant,
              "Variable is not a constant. It has type %1% instead. Has the test object %2% "
              "been evaluated?",
              high->type->node_type_name(), getObjectName());
    return constant;
}

const Range *Range::evaluate(const Model &model, bool doComplete) const {
    const auto *evaluatedLow = model.evaluate(low, doComplete);
    const auto *evaluatedHigh = model.evaluate(high, doComplete);
    return new Range(getKey(), evaluatedLow, evaluatedHigh);
}

cstring Range::getObjectName() const { return "Range"_cs; }

bool Range::isEqualTo(const TableMatch *other) const {
    const auto *o = other->to<Range>();
    return o && getEvaluatedLow()->value == o->getEvaluatedLow()->value &&
           getEvaluatedHigh()->value == o->getEvaluatedHigh()->value;
}

const IR::Expression *Range::buildTableKeyNeqConstraint(cstring tbl, cstring key) const {
    // Variable names mirror TofinoTableStepper::computeTargetMatchType
    cstring minName = tbl + "_range_min_" + key;
    cstring maxName = tbl + "_range_max_" + key;
    const auto *minVar = ToolsVariables::getSymbolicVariable(getEvaluatedLow()->type, minName);
    const auto *maxVar = ToolsVariables::getSymbolicVariable(getEvaluatedHigh()->type, maxName);
    return new IR::LOr(new IR::Neq(minVar, getEvaluatedLow()),
                       new IR::Neq(maxVar, getEvaluatedHigh()));
}

const IR::Expression *Range::buildTableKeyEqConstraint(cstring tbl, cstring key) const {
    // Variable names mirror TofinoTableStepper::computeTargetMatchType
    cstring minName = tbl + "_range_min_" + key;
    cstring maxName = tbl + "_range_max_" + key;
    const auto *minVar = ToolsVariables::getSymbolicVariable(getEvaluatedLow()->type, minName);
    const auto *maxVar = ToolsVariables::getSymbolicVariable(getEvaluatedHigh()->type, maxName);
    return new IR::LAnd(new IR::Equ(minVar, getEvaluatedLow()),
                        new IR::Equ(maxVar, getEvaluatedHigh()));
}

const IR::Expression *Range::buildPacketFieldNeqConstraint(
    const IR::Expression *pktField) const {
    return new IR::LOr(new IR::Lss(pktField, getEvaluatedLow()),
                       new IR::Grt(pktField, getEvaluatedHigh()));
}

const IR::Constant *Range::getRepresentativeValue() const { return getEvaluatedLow(); }

}  // namespace P4::P4Tools::Symbex::Tofino
