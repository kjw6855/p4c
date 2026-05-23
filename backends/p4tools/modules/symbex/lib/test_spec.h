#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_SPEC_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_SPEC_H_

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "ir/ir.h"
#include "lib/cstring.h"

#include "backends/p4tools/modules/symbex/lib/test_object.h"

namespace P4::P4Tools::Symbex {

/// This file defines a series of test objects which, in sum, produce an abstract test
/// specification.
/// This test specification is reified into a concrete test specification by the
/// individual test back ends of a target extension.

/* =========================================================================================
 *  Packet
 * ========================================================================================= */

class Packet : public TestObject {
 private:
    /// The port associated with this packet.
    int port;

    /// The actual content of the packet.
    const IR::Expression *payload;

    /// The mask of this packet.
    const IR::Expression *payloadIgnoreMask;

 public:
    Packet(int port, const IR::Expression *payload, const IR::Expression *payloadIgnoreMask);

    [[nodiscard]] const Packet *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    /// @returns the port that is associated with this packet.
    [[nodiscard]] int getPort() const;

    /// @returns the payload of the packet, which, at this point, needs to be a constant.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedPayload() const;

    /// @returns the don't care mask of the packet, which, at this point, needs to be a constant.
    /// If a bit is set in the @payloadIgnoreMask, the corresponding bit in @payload
    /// is ignored.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedPayloadMask() const;

    DECLARE_TYPEINFO(Packet, TestObject);
};

/* =========================================================================================
 *  Table Test Objects
 * ========================================================================================= */

class ActionArg : public TestObject {
 private:
    /// The original parameter associated with this object.
    const IR::Parameter *param;

    /// Value being supplied.
    const IR::Expression *value;

 public:
    ActionArg(const IR::Parameter *param, const IR::Expression *value);

    [[nodiscard]] const ActionArg *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    [[nodiscard]] const IR::Parameter *getActionParam() const;

    /// @returns the parameter name associated with the action argument.
    [[nodiscard]] cstring getActionParamName() const;

    /// @returns input argument value, which at this point needs to be a constant.
    /// If the value is a bool, it is converted into a constant.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedValue() const;

    DECLARE_TYPEINFO(ActionArg, TestObject);
};

class ActionCall : public TestObject {
 private:
    /// The name of the action. If not otherwise specified, this is the control plane name.
    cstring identifier;

    /// The action that is associated with this object.
    const IR::P4Action *action;

    /// Action arguments.
    const std::vector<ActionArg> args;

 public:
    ActionCall(cstring identifier, const IR::P4Action *action, std::vector<ActionArg> args);

    ActionCall(const IR::P4Action *action, std::vector<ActionArg> args);

    [[nodiscard]] const ActionCall *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    /// @returns the name of the action that is being called. If not otherwise specified, this is
    /// the control plane name.
    [[nodiscard]] cstring getActionName() const;

    /// @returns the action that is associated with this object.
    [[nodiscard]] const IR::P4Action *getAction() const;

    /// @returns the arguments of this particular call.
    [[nodiscard]] const std::vector<ActionArg> *getArgs() const;

    DECLARE_TYPEINFO(ActionCall, TestObject);
};

class TableMatch : public TestObject {
 private:
    /// The key associated with this object.
    const IR::KeyElement *key;

 public:
    explicit TableMatch(const IR::KeyElement *key);

    /// @returns the key associated with this object.
    [[nodiscard]] const IR::KeyElement *getKey() const;

    DECLARE_TYPEINFO(TableMatch, TestObject);
};

using TableMatchMap = std::map<cstring, const TableMatch *>;

class Ternary : public TableMatch {
 private:
    /// The actual match value.
    const IR::Expression *value;

    /// The mask is applied using binary and operator when matching with a key.
    const IR::Expression *mask;

 public:
    explicit Ternary(const IR::KeyElement *key, const IR::Expression *value,
                     const IR::Expression *mask);

    [[nodiscard]] const Ternary *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    /// @returns the value of the ternary object, which is matched with the key. At this point the
    /// value needs to be a constant.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedValue() const;

    /// @returns the mask of the ternary object. At this point the
    /// value needs to be a constant.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedMask() const;

    DECLARE_TYPEINFO(Ternary, TableMatch);
};

class LPM : public TableMatch {
 private:
    /// How value the key is matched with.
    const IR::Expression *value;

    /// How much of the value to match.
    const IR::Expression *prefixLength;

 public:
    explicit LPM(const IR::KeyElement *key, const IR::Expression *value,
                 const IR::Expression *prefixLength);

    [[nodiscard]] const LPM *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    /// @returns the value of the LPM object, which is matched with the key. At this point the
    /// value needs to be a constant.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedValue() const;

    /// @returns the prefix of LPM, which describes how many bits of the value are matched. At this
    /// point the prefix is expected to be a constant.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedPrefixLength() const;

    DECLARE_TYPEINFO(LPM, TableMatch);
};

class Exact : public TableMatch {
 private:
    /// The value the key is matched with.
    const IR::Expression *value;

 public:
    explicit Exact(const IR::KeyElement *key, const IR::Expression *value);

    [[nodiscard]] const Exact *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    /// @returns the match value. It is expected to be a constant at this point.
    /// A BUG is thrown otherwise.
    [[nodiscard]] const IR::Constant *getEvaluatedValue() const;

    DECLARE_TYPEINFO(Exact, TableMatch);
};

class TableRule : public TestObject {
 private:
    /// Each element in the map is the control plane name of the key paired with its match rule.
    const TableMatchMap matches;

    /// The priority of this entry. This is required for STF back ends when matching ternary.
    int priority;
    /// The action that is called when the key matches.
    const ActionCall action;
    /// The expiry time of the entry. We currently set this to not expire.
    int ttl;

 public:
    TableRule(TableMatchMap matches, int priority, ActionCall action, int ttl);

    [[nodiscard]] const TableRule *evaluate(const Model &model, bool doComplete) const override;

    [[nodiscard]] cstring getObjectName() const override;

    /// @returns the list of keys that need to match to execute the action.
    [[nodiscard]] const TableMatchMap *getMatches() const;

    /// @returns the priority of this entry.
    [[nodiscard]] int getPriority() const;

    /// @returns action that is called when the key matches.
    [[nodiscard]] const ActionCall *getActionCall() const;

    /// @returns the time-to-live of this particular entry.
    [[nodiscard]] int getTTL() const;

    DECLARE_TYPEINFO(TableRule, TestObject);
};

class TableConfig : public TestObject {
 private:
    /// The original table associated with this object.
    const IR::P4Table *table;

    /// Vector of TableRules.
    const std::vector<TableRule> rules;

    /// A map of table properties. For example, an action profile may be part of a table property.
    TestObjectMap tableProperties;

 public:
    explicit TableConfig(const IR::P4Table *table, std::vector<TableRule> rules);

    explicit TableConfig(const IR::P4Table *table, std::vector<TableRule> rules,
                         TestObjectMap tableProperties);

    [[nodiscard]] const IR::P4Table *getTable() const;

    [[nodiscard]] cstring getObjectName() const override;

    [[nodiscard]] const TableConfig *evaluate(const Model &model, bool doComplete) const override;

    /// @returns the table rules of this table.
    [[nodiscard]] const std::vector<TableRule> *getRules() const;

    /// @returns the properties associated with this table.
    [[nodiscard]] const TestObjectMap *getProperties() const;

    /// @returns a particular property based on @param propertyName.
    /// If @param checked is true, this will throw a BUG if @param propertyName is not in the list.
    [[nodiscard]] const TestObject *getProperty(cstring propertyName, bool checked) const;

    /// Add a table property to the table.
    void addTableProperty(cstring propertyName, const TestObject *property);

    DECLARE_TYPEINFO(TableConfig, TestObject);
};

/* =========================================================================================
 * Test Specification
 * ========================================================================================= */

class TestSpec;  // forward declaration for TamperingTestSpec

/// Test specification for the three-packet tampering scenario.
/// Bundles two symbex TestSpec objects (Phase 1 and Phase 2) together with a flag that
/// indicates whether the read phases must produce an output packet (i.e., the SOChain's
/// read vertices include __EXIT__ leaves, meaning the packet is not dropped).
/// Phase 3 is purely dynamic: the test script replays Phase 1's input packet after
/// Phase 2 has written the attacker-chosen value to the register.
///
/// Execution order on the same set of table entries (taken from spec1):
///   spec1 — Phase 1: reads the original register value
///   spec2 — Phase 2: writes the tampered register value
///   (Phase 3 dynamic: replays spec1's input; deviation from spec1's output = tampering confirmed)
class TamperingTestSpec {
 public:
    /// Full test spec for Phase 1 (read original value).
    const TestSpec *spec1;
    /// Full test spec for Phase 2 (write tampered value).
    const TestSpec *spec2;

    /// True when the SOChain's readVertices include __EXIT__ leaves, meaning the read
    /// phases are expected to emit an output packet (i.e., packet is not dropped).
    bool readPathHasExit;

    /// Attacker-chosen register values from Phase 2 (register name → concrete TestObject).
    /// Used to emit affected_register fields in the test output so the test script can
    /// verify the register was written with the expected value after Phase 2.
    std::map<cstring, const TestObject *> attackerRegisterValues;

    TamperingTestSpec(const TestSpec *s1, const TestSpec *s2, bool hasExit,
                      std::map<cstring, const TestObject *> attackerRegVals = {})
        : spec1(s1), spec2(s2), readPathHasExit(hasExit),
          attackerRegisterValues(std::move(attackerRegVals)) {}
};

class TestSpec {
 private:
    /// The input packet of the test.
    const Packet ingressPacket;

    /// The expected output packet of this test. Only single output packets are supported.
    /// Additional output packets need to be encoded as test objects.
    const std::optional<Packet> egressPacket;

    /// The traces that have been collected while the interpreter stepped through the program.
    const std::vector<std::reference_wrapper<const TraceEvent>> traces;

    /// A map of additional properties associated with this test specification.
    /// For example, tables, registers, or action profiles.
    std::map<cstring, TestObjectMap> testObjects;

 public:
    TestSpec(Packet ingressPacket, std::optional<Packet> egressPacket,
             std::vector<std::reference_wrapper<const TraceEvent>> traces);

    /// Add a test object to the test specification with @param category as the object category
    /// (for example, "tables", "registers", "action_profiles") and objectLabel as the concrete,
    /// individual label of the object.
    void addTestObject(cstring category, cstring objectLabel, const TestObject *object);

    /// @returns a test object for the given category and objectlabel.
    /// Returns a BUG if checked is true and the object is not found.
    [[nodiscard]] const TestObject *getTestObject(cstring category, cstring objectLabel,
                                                  bool checked) const;

    /// @returns the test object using the provided category and object label. If
    /// @param checked is enabled, a BUG is thrown if the object label does not exist.
    /// Also casts the test object to the specified type. If the type does not match, a BUG is
    /// thrown.
    template <class T>
    [[nodiscard]] auto *getTestObject(cstring category, cstring objectLabel, bool checked) const {
        const auto *testObject = getTestObject(category, objectLabel, checked);
        return testObject->checkedTo<T>();
    }

    /// @returns the map of test objects for a given category.
    [[nodiscard]] TestObjectMap getTestObjectCategory(cstring category) const;

    /// @returns the input packet for this test.
    [[nodiscard]] const Packet *getIngressPacket() const;

    /// @returns the list of tables that need to be configured for this test.
    [[nodiscard]] const std::map<cstring, const TableConfig> *getTables() const;

    /// @returns the expected output packet for this test.
    [[nodiscard]] std::optional<const Packet *> getEgressPacket() const;

    /// @returns the list of traces that has been executed
    [[nodiscard]] const std::vector<std::reference_wrapper<const TraceEvent>> *getTraces() const;

    /// Priority definitions for LPM and ternary entries.
    static constexpr int NO_PRIORITY = -1;
    static constexpr int LOW_PRIORITY = 10;
    static constexpr int HIGH_PRIORITY = 100;
    static constexpr int TTL = 0;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_SPEC_H_ */
