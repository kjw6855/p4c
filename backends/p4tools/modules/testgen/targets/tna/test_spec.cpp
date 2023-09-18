#include "backends/p4tools/modules/testgen/targets/tna/test_spec.h"

#include "backends/p4tools/common/lib/model.h"
#include "lib/exceptions.h"

#include "backends/p4tools/modules/testgen/lib/test_spec.h"

namespace P4Tools::P4Testgen::Tna {

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

cstring IndexExpression::getObjectName() const { return "IndexExpression"; }

const IndexExpression *IndexExpression::evaluate(const Model &model) const {
    const auto *evaluatedIndex = model.evaluate(index);
    const auto *evaluatedValue = model.evaluate(value);
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
 *  TnaRegister
 * ========================================================================================= */

IndexMap::IndexMap(const IR::Expression *initialValue) : initialValue(initialValue) {}

void IndexMap::writeToIndex(const IR::Expression *index, const IR::Expression *value) {
    indexConditions.emplace_back(index, value);
}

const IR::Expression *IndexMap::getInitialValue() const { return initialValue; }

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

const IR::Constant *IndexMap::getEvaluatedInitialValue() const {
    const auto *constant = initialValue->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return constant;
}

/* =========================================================================================
 *  TnaRegisterValue
 * ========================================================================================= */

TnaRegisterValue::TnaRegisterValue(const IR::Expression *initialValue)
    : IndexMap(initialValue) {}

cstring TnaRegisterValue::getObjectName() const { return "TnaRegisterValue"; }

const TnaRegisterValue *TnaRegisterValue::evaluate(const Model &model) const {
    const auto *evaluatedValue = model.evaluate(getInitialValue());
    auto *evaluatedRegisterValue = new TnaRegisterValue(evaluatedValue);
    for (const auto &cond : indexConditions) {
        const auto *evaluatedCond = cond.evaluate(model);
        evaluatedRegisterValue->writeToIndex(evaluatedCond->getEvaluatedIndex(),
                                             evaluatedCond->getEvaluatedValue());
    }
    return evaluatedRegisterValue;
}

/* =========================================================================================
 *  TnaMeterValue
 * ========================================================================================= */

TnaMeterValue::TnaMeterValue(const IR::Expression *initialValue, bool isDirect)
    : IndexMap(initialValue), isDirect(isDirect) {}

cstring TnaMeterValue::getObjectName() const { return "TnaMeterValue"; }

const TnaMeterValue *TnaMeterValue::evaluate(const Model &model) const {
    const auto *evaluatedValue = model.evaluate(getInitialValue());
    auto *evaluatedMeterValue = new TnaMeterValue(evaluatedValue, isDirect);
    for (const auto &cond : indexConditions) {
        const auto *evaluatedCond = cond.evaluate(model);
        evaluatedMeterValue->writeToIndex(evaluatedCond->getEvaluatedIndex(),
                                          evaluatedCond->getEvaluatedValue());
    }
    return evaluatedMeterValue;
}

bool TnaMeterValue::isDirectMeter() const { return isDirect; }

/* =========================================================================================
 *  TnaActionProfile
 * ========================================================================================= */

const std::vector<std::pair<cstring, std::vector<ActionArg>>>
    *TnaActionProfile::getActions() const {
    return &actions;
}

TnaActionProfile::TnaActionProfile(const IR::IDeclaration *profileDecl)
    : profileDecl(profileDecl) {}

cstring TnaActionProfile::getObjectName() const { return profileDecl->controlPlaneName(); }

const IR::IDeclaration *TnaActionProfile::getProfileDecl() const { return profileDecl; }

void TnaActionProfile::addToActionMap(cstring actionName,
                                              std::vector<ActionArg> actionArgs) {
    actions.emplace_back(actionName, actionArgs);
}
size_t TnaActionProfile::getActionMapSize() const { return actions.size(); }

const TnaActionProfile *TnaActionProfile::evaluate(const Model &model) const {
    auto *profile = new TnaActionProfile(profileDecl);
    for (const auto &actionTuple : actions) {
        auto actionArgs = actionTuple.second;
        std::vector<ActionArg> evaluatedArgs;
        evaluatedArgs.reserve(actionArgs.size());
        for (const auto &actionArg : actionArgs) {
            evaluatedArgs.emplace_back(*actionArg.evaluate(model));
        }
        profile->addToActionMap(actionTuple.first, evaluatedArgs);
    }
    return profile;
}

/* =========================================================================================
 *  TnaActionSelector
 * ========================================================================================= */

TnaActionSelector::TnaActionSelector(const IR::IDeclaration *selectorDecl,
                                                     const TnaActionProfile *actionProfile)
    : selectorDecl(selectorDecl), actionProfile(actionProfile) {}

cstring TnaActionSelector::getObjectName() const { return "TnaActionSelector"; }

const IR::IDeclaration *TnaActionSelector::getSelectorDecl() const { return selectorDecl; }

const TnaActionProfile *TnaActionSelector::getActionProfile() const {
    return actionProfile;
}

const TnaActionSelector *TnaActionSelector::evaluate(const Model &model) const {
    const auto *evaluatedProfile = actionProfile->evaluate(model);
    return new TnaActionSelector(selectorDecl, evaluatedProfile);
}

/* =========================================================================================
 *  TnaCloneInfo
 * ========================================================================================= */

TnaCloneInfo::TnaCloneInfo(const IR::Expression *sessionId,
                                           TNAConstants::CloneType cloneType,
                                           const ExecutionState &clonedState,
                                           std::optional<int> preserveIndex)
    : sessionId(sessionId),
      cloneType(cloneType),
      clonedState(clonedState),
      preserveIndex(preserveIndex) {}

cstring TnaCloneInfo::getObjectName() const { return "TnaCloneInfo"; }

TNAConstants::CloneType TnaCloneInfo::getCloneType() const { return cloneType; }

const IR::Expression *TnaCloneInfo::getSessionId() const { return sessionId; }

const ExecutionState &TnaCloneInfo::getClonedState() const { return clonedState; }

std::optional<int> TnaCloneInfo::getPreserveIndex() const { return preserveIndex; }

const TnaCloneInfo *TnaCloneInfo::evaluate(const Model & /*model*/) const {
    P4C_UNIMPLEMENTED("Evaluate is not implemented for this test object.");
}

/* =========================================================================================
 *  TnaCloneSpec
 * ========================================================================================= */

TnaCloneSpec::TnaCloneSpec(const IR::Expression *sessionId,
                                           const IR::Expression *clonePort, bool isClone)
    : sessionId(sessionId), clonePort(clonePort), isClone(isClone) {}

cstring TnaCloneSpec::getObjectName() const { return "TnaCloneSpec"; }

const IR::Expression *TnaCloneSpec::getClonePort() const { return clonePort; }

const IR::Expression *TnaCloneSpec::getSessionId() const { return sessionId; }

const IR::Constant *TnaCloneSpec::getEvaluatedClonePort() const {
    const auto *constant = clonePort->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return constant;
}

const IR::Constant *TnaCloneSpec::getEvaluatedSessionId() const {
    const auto *constant = sessionId->to<IR::Constant>();
    BUG_CHECK(constant, "Variable is not a constant, has the test object %1% been evaluated?",
              getObjectName());
    return constant;
}

const TnaCloneSpec *TnaCloneSpec::evaluate(const Model &model) const {
    return new TnaCloneSpec(model.evaluate(sessionId), model.evaluate(clonePort), isClone);
}

bool TnaCloneSpec::isClonedPacket() const { return isClone; }

/* =========================================================================================
 * Table Key Match Types
 * ========================================================================================= */

Optional::Optional(const IR::KeyElement *key, const IR::Expression *val, bool addMatch)
    : TableMatch(key), value(val), addMatch(addMatch) {}

const IR::Constant *Optional::getEvaluatedValue() const {
    const auto *constant = value->to<IR::Constant>();
    BUG_CHECK(constant,
              "Variable is not a constant. It has type %1% instead. Has the test object %2% "
              "been evaluated?",
              value->type->node_type_name(), getObjectName());
    return constant;
}

const Optional *Optional::evaluate(const Model &model) const {
    const auto *evaluatedValue = model.evaluate(value);
    return new Optional(getKey(), evaluatedValue, addMatch);
}

cstring Optional::getObjectName() const { return "Optional"; }

bool Optional::addAsExactMatch() const { return addMatch; }

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

const Range *Range::evaluate(const Model &model) const {
    const auto *evaluatedLow = model.evaluate(low);
    const auto *evaluatedHigh = model.evaluate(high);
    return new Range(getKey(), evaluatedLow, evaluatedHigh);
}

/* =========================================================================================
 *  MetadataCollection
 * ========================================================================================= */

MetadataCollection::MetadataCollection() = default;

cstring MetadataCollection::getObjectName() const { return "MetadataCollection"; }

const MetadataCollection *MetadataCollection::evaluate(const Model & /*model*/) const {
    P4C_UNIMPLEMENTED("%1% has no implementation for \"evaluate\".", getObjectName());
}

const std::map<cstring, const IR::Literal *> &MetadataCollection::getMetadataFields() const {
    return metadataFields;
}

const IR::Literal *MetadataCollection::getMetadataField(cstring name) {
    return metadataFields.at(name);
}

void MetadataCollection::addMetaDataField(cstring name, const IR::Literal *metadataField) {
    metadataFields[name] = metadataField;
}

cstring Range::getObjectName() const { return "Range"; }

}  // namespace P4Tools::P4Testgen::Tna
