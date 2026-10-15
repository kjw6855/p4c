#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_snapshot.h"

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/common/lib/logging.h"
#include "backends/p4tools/common/lib/variables.h"
#include "backends/p4tools/modules/symbex/lib/final_state.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/p4symbex.pb.h"
#include "backends/p4tools/modules/symbex/p4symbex_bfrt.pb.h"
#include "lib/error.h"
#include "p4/config/v1/p4info.pb.h"

namespace P4::P4Tools::Symbex {

namespace {

/// Unsigned big-endian bytes, as P4Runtime and BfRt encode values.
big_int bytesToInt(const std::string &bytes) {
    big_int v = 0;
    for (unsigned char c : bytes) v = (v << 8) | big_int(c);
    return v;
}

/// Control-plane names: drop the leading '.' p4c puts on top-level declarations.
std::string_view bare(std::string_view name) {
    while (!name.empty() && name.front() == '.') name.remove_prefix(1);
    return name;
}

/// True when snapshot name @p snap names the same object as IR control-plane name @p cp: equal,
/// or @p cp is a dotted suffix of @p snap (BfRt prefixes the pipeline name, e.g. "pipe.").
bool nameMatches(std::string_view snap, std::string_view cp) {
    snap = bare(snap);
    cp = bare(cp);
    if (snap == cp) return true;
    return snap.size() > cp.size() && snap.substr(snap.size() - cp.size()) == cp &&
           snap[snap.size() - cp.size() - 1] == '.';
}

/// bfrt_python spells field names as Python identifiers ("egress_port_7_0_"); compare names with
/// every non-alphanumeric character folded to '_' as a fallback.
std::string folded(std::string_view name) {
    std::string out(name);
    for (auto &c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return out;
}

/// BfRt fixed tables ("$mirror.cfg", "$pre.mgid", "pipe.SwitchIngressParser.$PORT_METADATA"):
/// some dotted component starts with '$'.
bool isInternalBfRtTable(std::string_view name) {
    size_t start = 0;
    while (start < name.size()) {
        if (name[start] == '$') return true;
        auto dot = name.find('.', start);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return false;
}

std::vector<big_int> p4DataValues(const p4::v1::P4Data &data) {
    std::vector<big_int> out;
    if (data.has_bitstring()) {
        out.push_back(bytesToInt(data.bitstring()));
    } else if (data.has_bool_()) {
        out.push_back(data.bool_() ? 1 : 0);
    } else if (data.has_struct_()) {
        for (const auto &m : data.struct_().members()) {
            auto inner = p4DataValues(m);
            out.insert(out.end(), inner.begin(), inner.end());
        }
    } else if (data.has_tuple()) {
        for (const auto &m : data.tuple().members()) {
            auto inner = p4DataValues(m);
            out.insert(out.end(), inner.begin(), inner.end());
        }
    }
    return out;
}

CpSnapshotRule ruleFromP4Runtime(const p4::v1::TableEntry &te) {
    CpSnapshotRule rule;
    for (const auto &fm : te.match()) {
        CpSnapshotKey key;
        if (fm.has_exact()) {
            key.value = bytesToInt(fm.exact().value());
        } else if (fm.has_ternary()) {
            key.kind = CpSnapshotKey::Kind::Ternary;
            key.value = bytesToInt(fm.ternary().value());
            key.mask = bytesToInt(fm.ternary().mask());
        } else if (fm.has_lpm()) {
            key.kind = CpSnapshotKey::Kind::Lpm;
            key.value = bytesToInt(fm.lpm().value());
            key.prefixLen = fm.lpm().prefix_len();
        } else if (fm.has_range()) {
            key.kind = CpSnapshotKey::Kind::Range;
            key.value = bytesToInt(fm.range().low());
            key.high = bytesToInt(fm.range().high());
        } else if (fm.has_optional()) {
            key.kind = CpSnapshotKey::Kind::Optional;
            key.value = bytesToInt(fm.optional().value());
        } else {
            error(ErrorType::ERR_UNSUPPORTED, "--cp-snapshot: table %1% key %2%: unsupported match",
                  te.table_name(), fm.field_name());
            continue;
        }
        rule.keys[cstring(fm.field_name())] = key;
    }
    if (te.action().has_action()) {
        const auto &a = te.action().action();
        rule.action = cstring(a.action_name());
        for (const auto &p : a.params()) rule.params[cstring(p.param_name())] = bytesToInt(p.value());
    } else if (te.has_action()) {
        error(ErrorType::ERR_UNSUPPORTED,
              "--cp-snapshot: table %1%: action profiles/selectors are not supported",
              te.table_name());
    }
    rule.priority = te.priority();
    return rule;
}

CpSnapshotRule ruleFromBfRt(const symbex::tofino::BfRtEntity &e) {
    CpSnapshotRule rule;
    rule.action = cstring(e.action_name());
    rule.priority = -static_cast<int64_t>(e.priority());
    for (const auto &k : e.key()) {
        if (k.field_name() == "$MATCH_PRIORITY") {
            rule.priority = -static_cast<int64_t>(bytesToInt(k.exact().value()));
            continue;
        }
        CpSnapshotKey key;
        if (k.has_exact()) {
            key.value = bytesToInt(k.exact().value());
        } else if (k.has_ternary()) {
            key.kind = CpSnapshotKey::Kind::Ternary;
            key.value = bytesToInt(k.ternary().value());
            key.mask = bytesToInt(k.ternary().mask());
        } else if (k.has_lpm()) {
            key.kind = CpSnapshotKey::Kind::Lpm;
            key.value = bytesToInt(k.lpm().value());
            key.prefixLen = static_cast<int>(k.lpm().prefix_len());
        } else if (k.has_range()) {
            key.kind = CpSnapshotKey::Kind::Range;
            key.value = bytesToInt(k.range().low());
            key.high = bytesToInt(k.range().high());
        } else if (k.has_optional()) {
            key.kind = CpSnapshotKey::Kind::Optional;
            key.value = bytesToInt(k.optional().value());
        }
        rule.keys[cstring(k.field_name())] = key;
    }
    for (const auto &d : e.data()) {
        // BfRt names action data by parameter name; register data ("<reg>.f1") is handled by the
        // caller.
        rule.params[cstring(d.field_name())] = bytesToInt(d.value());
    }
    return rule;
}

}  // namespace

std::optional<CpSnapshot> CpSnapshot::load(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        error(ErrorType::ERR_IO, "--cp-snapshot: cannot open %1%", path);
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    auto msg = std::make_shared<symbex::CpSnapshot>();
    if (!google::protobuf::TextFormat::ParseFromString(buf.str(), msg.get())) {
        error(ErrorType::ERR_INVALID, "--cp-snapshot: %1% is not a symbex.CpSnapshot textproto",
              path);
        return std::nullopt;
    }
    CpSnapshot snap;
    snap.proto_ = msg;
    snap.target_ = cstring(msg->target());
    snap.program_ = cstring(msg->program());

    for (const auto &entity : msg->entities()) {
        if (entity.has_table_entry()) {
            const auto &te = entity.table_entry();
            auto &table = snap.tables_[cstring(te.table_name())];
            if (te.is_default_action()) {
                table.defaultAction = ruleFromP4Runtime(te);
            } else {
                table.rules.push_back(ruleFromP4Runtime(te));
            }
        } else if (entity.has_register_entry()) {
            const auto &re = entity.register_entry();
            snap.registersById_[re.register_id()][re.index().index()] = p4DataValues(re.data());
        }
        // PRE entries (multicast groups, clone sessions) are kept in the proto for emission.
    }
    for (const auto &e : msg->bfrt_entities()) {
        const bool isRegister =
            std::any_of(e.key().begin(), e.key().end(),
                        [](const auto &k) { return k.field_name() == "$REGISTER_INDEX"; });
        if (isRegister) {
            big_int index = 0;
            for (const auto &k : e.key()) {
                if (k.field_name() == "$REGISTER_INDEX") index = bytesToInt(k.exact().value());
            }
            std::vector<big_int> values;
            for (const auto &d : e.data()) values.push_back(bytesToInt(d.value()));
            snap.registers_[cstring(e.table_name())][index] = values;
            continue;
        }
        // $mirror.cfg, $pre.*, $PORT_METADATA: replay-only state, kept in the proto for emission.
        if (isInternalBfRtTable(e.table_name())) continue;
        // A RegisterParam: no key, no action, a single data field ("value").
        if (e.key_size() == 0 && e.action_name().empty() && e.data_size() == 1) {
            snap.registerParams_[cstring(e.table_name())] = bytesToInt(e.data(0).value());
            continue;
        }
        auto &table = snap.tables_[cstring(e.table_name())];
        if (e.is_default_entry()) {
            table.defaultAction = ruleFromBfRt(e);
        } else {
            table.rules.push_back(ruleFromBfRt(e));
        }
    }
    // Multicast groups -> replica ports, for the output-port oracle.
    std::map<int, std::set<int>> nodePorts;
    std::map<int, std::vector<int>> mgidNodes;
    for (const auto &e : msg->bfrt_entities()) {
        if (e.table_name() != "$pre.node" && e.table_name() != "$pre.mgid") continue;
        if (e.key_size() == 0 || !e.key(0).has_exact()) continue;
        const int id = static_cast<int>(bytesToInt(e.key(0).exact().value()));
        for (const auto &d : e.data()) {
            if (e.table_name() == "$pre.node" && d.field_name() == "$DEV_PORT") {
                for (auto p : d.int_arr_val()) nodePorts[id].insert(static_cast<int>(p));
            }
            if (e.table_name() == "$pre.mgid" && d.field_name() == "$MULTICAST_NODE_ID") {
                for (auto n : d.int_arr_val()) mgidNodes[id].push_back(static_cast<int>(n));
            }
        }
    }
    for (const auto &[mgid, nodes] : mgidNodes) {
        for (int n : nodes) {
            const auto &ports = nodePorts[n];
            snap.multicastPorts_[mgid].insert(ports.begin(), ports.end());
        }
    }
    for (const auto &entity : msg->entities()) {
        if (!entity.has_packet_replication_engine_entry()) continue;
        const auto &pre = entity.packet_replication_engine_entry();
        if (!pre.has_multicast_group_entry()) continue;
        const auto &g = pre.multicast_group_entry();
        auto &ports = snap.multicastPorts_[static_cast<int>(g.multicast_group_id())];
        for (const auto &r : g.replicas()) {
            ports.insert(r.has_port() ? static_cast<int>(bytesToInt(r.port()))
                                      : static_cast<int>(r.egress_port()));
        }
    }
    for (const auto &port : msg->topology().port()) {
        if (port.role() == symbex::PORT_ROLE_HOST) snap.hostPorts_.insert(int(port.port()));
        if (port.role() == symbex::PORT_ROLE_ATTACKER) snap.attackerPorts_.insert(int(port.port()));
    }
    if (snap.hasTopology() && (snap.hostPorts_.empty() || snap.attackerPorts_.empty())) {
        error(ErrorType::ERR_INVALID,
              "--cp-snapshot: the topology must name at least one HOST and one ATTACKER port");
        return std::nullopt;
    }
    size_t rules = 0;
    for (const auto &[name, t] : snap.tables_) rules += t.rules.size();
    printInfo("[CP snapshot] %1%: %2% tables, %3% rules, %4% registers, host ports %5%, attacker "
              "ports %6%",
              path, snap.tables_.size(), rules, snap.registers_.size() + snap.registersById_.size(),
              snap.hostPorts_.size(), snap.attackerPorts_.size());
    return snap;
}

const CpSnapshotTable *CpSnapshot::table(cstring cpName) const {
    const CpSnapshotTable *found = nullptr;
    cstring foundName;
    for (const auto &[name, t] : tables_) {
        if (!nameMatches(name.string_view(), cpName.string_view())) continue;
        if (found != nullptr) {
            error(ErrorType::ERR_INVALID, "--cp-snapshot: table %1% matches both %2% and %3%",
                  cpName, foundName, name);
            return nullptr;
        }
        found = &t;
        foundName = name;
    }
    if (found != nullptr) claimedTables_.insert(foundName);
    return found;
}

std::set<cstring> CpSnapshot::unclaimedTables() const {
    std::set<cstring> out;
    for (const auto &[name, t] : tables_) {
        if (claimedTables_.count(name) == 0) out.insert(name);
    }
    return out;
}

const std::map<big_int, std::vector<big_int>> *CpSnapshot::registerCells(cstring cpName) const {
    for (const auto &[name, cells] : registers_) {
        if (nameMatches(name.string_view(), cpName.string_view())) return &cells;
    }
    return nullptr;
}

std::optional<std::set<int>> CpSnapshot::multicastPorts(int mgid) const {
    auto it = multicastPorts_.find(mgid);
    if (it == multicastPorts_.end()) return std::nullopt;
    return it->second;
}

std::optional<big_int> CpSnapshot::registerParam(cstring cpName) const {
    for (const auto &[name, value] : registerParams_) {
        if (nameMatches(name.string_view(), cpName.string_view())) return value;
    }
    return std::nullopt;
}

void CpSnapshot::bindP4Info(const p4::config::v1::P4Info &p4Info) {
    for (const auto &reg : p4Info.registers()) {
        auto it = registersById_.find(reg.preamble().id());
        if (it == registersById_.end()) continue;
        registers_[cstring(reg.preamble().name())] = std::move(it->second);
        registersById_.erase(it);
    }
    for (const auto &[id, cells] : registersById_) {
        error(ErrorType::ERR_NOT_FOUND, "--cp-snapshot: register id %1% is not in the P4Info", id);
    }
    registersById_.clear();
}

CpSnapshot *loadedCpSnapshot() {
    static std::optional<CpSnapshot> cache;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const auto &paths = SymbexOptions::get().cpSnapshotPaths;
        if (!paths.empty()) cache = CpSnapshot::load(paths.front());
    }
    return cache.has_value() ? &cache.value() : nullptr;
}

namespace {

std::string indented(const std::string &text) {
    std::stringstream out;
    std::stringstream in(text);
    std::string line;
    while (std::getline(in, line)) out << "  " << line << "\n";
    return out.str();
}

}  // namespace

std::string cpSnapshotTxtpbBlock(bool bfrt) {
    const auto *snap = loadedCpSnapshot();
    if (snap == nullptr) return {};
    const auto &msg = snap->proto();
    std::stringstream out;
    out << "\n# cp-snapshot: program " << msg.program() << ", snapshot " << msg.snapshot_id()
        << ", captured " << msg.captured_at() << "\n";
    std::string text;
    if (bfrt) {
        for (const auto &e : msg.bfrt_entities()) {
            google::protobuf::TextFormat::PrintToString(e, &text);
            out << "snapshot_entities {\n" << indented(text) << "}\n";
        }
    } else {
        for (const auto &e : msg.entities()) {
            google::protobuf::TextFormat::PrintToString(e, &text);
            out << "snapshot_entities {\n" << indented(text) << "}\n";
        }
    }
    return out.str();
}

/* =============================================================================================
 *  ApplyCpSnapshot
 * ============================================================================================= */

namespace {

/// @p value as a constant of key/parameter type @p type (bit<N>, int<N> or bool).
const IR::Expression *constantOf(const IR::Type *type, big_int value) {
    if (type->is<IR::Type_Boolean>()) return IR::BoolLiteral::get(value != 0);
    const auto *bits = type->to<IR::Type_Bits>();
    BUG_CHECK(bits != nullptr, "--cp-snapshot: unsupported value type %1%", type);
    big_int max = (big_int(1) << bits->width_bits()) - 1;
    return IR::Constant::get(type, value & max);
}

int widthOf(const IR::Type *type) {
    if (type->is<IR::Type_Boolean>()) return 1;
    const auto *bits = type->to<IR::Type_Bits>();
    BUG_CHECK(bits != nullptr, "--cp-snapshot: unsupported key type %1%", type);
    return bits->width_bits();
}

cstring keyName(const IR::KeyElement *keyElement) {
    if (const auto *ann = keyElement->getAnnotation(IR::Annotation::nameAnnotation)) {
        return ann->getName();
    }
    return keyElement->expression->toString();
}

/// The snapshot key of @p rule for key element @p name: exact name first, then the
/// identifier-folded spelling bfrt_python produces.
const CpSnapshotKey *findKey(const CpSnapshotRule &rule, cstring name) {
    auto it = rule.keys.find(name);
    if (it != rule.keys.end()) return &it->second;
    const auto want = folded(name.string_view());
    for (const auto &[k, v] : rule.keys) {
        if (folded(k.string_view()) == want) return &v;
    }
    return nullptr;
}

/// The keyset expression for one key of a snapshot rule. A field the rule omits is a wildcard
/// (P4Runtime omits don't-care ternary/LPM/optional/range fields); an exact key must be present.
const IR::Expression *keysetOf(const IR::KeyElement *keyElement, const CpSnapshotKey *key,
                               cstring table) {
    const auto *type = keyElement->expression->type;
    const auto matchKind = keyElement->matchType->path->name.name;
    if (key == nullptr) {
        if (matchKind == "exact") {
            error(ErrorType::ERR_INVALID, "--cp-snapshot: table %1% rule has no value for exact key %2%",
                  table, keyName(keyElement));
        }
        return new IR::DefaultExpression(type);
    }
    const int width = widthOf(type);
    const big_int all = (big_int(1) << width) - 1;
    switch (key->kind) {
        case CpSnapshotKey::Kind::Exact:
        case CpSnapshotKey::Kind::Optional:
            return constantOf(type, key->value);
        case CpSnapshotKey::Kind::Ternary: {
            const big_int mask = key->mask & all;
            if (mask == 0) return new IR::DefaultExpression(type);
            if (mask == all) return constantOf(type, key->value);
            return new IR::Mask(constantOf(type, key->value & mask), constantOf(type, mask));
        }
        case CpSnapshotKey::Kind::Lpm: {
            if (key->prefixLen <= 0) return new IR::DefaultExpression(type);
            if (key->prefixLen >= width) return constantOf(type, key->value);
            const big_int mask = all ^ ((big_int(1) << (width - key->prefixLen)) - 1);
            return new IR::Mask(constantOf(type, key->value & mask), constantOf(type, mask));
        }
        case CpSnapshotKey::Kind::Range:
            if (key->value == 0 && (key->high & all) == all) return new IR::DefaultExpression(type);
            return new IR::Range(constantOf(type, key->value), constantOf(type, key->high));
    }
    BUG("unreachable");
}

/// The action-list call of @p table whose action has control-plane name @p action, with @p rule's
/// parameters as constant arguments; nullptr (after an error) when it cannot be built.
const IR::MethodCallExpression *actionCallOf(const IR::P4Table *table, const IR::P4Control *control,
                                             const CpSnapshotRule &rule) {
    const auto *actionList = table->getActionList();
    if (actionList == nullptr) return nullptr;
    for (const auto *ale : actionList->actionList) {
        const auto *call = ale->expression->to<IR::MethodCallExpression>();
        if (call == nullptr) continue;
        const auto *path = call->method->to<IR::PathExpression>();
        if (path == nullptr) continue;
        const IR::P4Action *action = nullptr;
        if (control != nullptr) {
            for (const auto *decl : control->controlLocals) {
                if (const auto *a = decl->to<IR::P4Action>();
                    a != nullptr && a->name == path->path->name) {
                    action = a;
                    break;
                }
            }
        }
        if (action == nullptr) continue;
        if (!nameMatches(rule.action.string_view(), action->controlPlaneName().string_view())) {
            continue;
        }
        auto *args = new IR::Vector<IR::Argument>();
        for (const auto *param : action->parameters->parameters) {
            if (param->direction != IR::Direction::None) {
                error(ErrorType::ERR_UNSUPPORTED,
                      "--cp-snapshot: table %1% action %2% has a directional parameter %3%",
                      table->controlPlaneName(), rule.action, param);
                return nullptr;
            }
            auto it = rule.params.find(param->controlPlaneName());
            if (it == rule.params.end()) it = rule.params.find(param->name.name);
            if (it == rule.params.end()) {
                error(ErrorType::ERR_INVALID,
                      "--cp-snapshot: table %1% action %2% has no value for parameter %3%",
                      table->controlPlaneName(), rule.action, param->controlPlaneName());
                return nullptr;
            }
            args->push_back(new IR::Argument(constantOf(param->type, it->second)));
        }
        auto *out = call->clone();
        out->arguments = args;
        return out;
    }
    std::stringstream names;
    for (const auto *ale : actionList->actionList) names << " " << ale->getName();
    error(ErrorType::ERR_NOT_FOUND, "--cp-snapshot: table %1% has no action %2% (actions:%3%)",
          table->controlPlaneName(), rule.action, names.str());
    return nullptr;
}

}  // namespace

const IR::Node *ApplyCpSnapshot::preorder(IR::P4Table *table) {
    prune();
    const cstring cpName = table->controlPlaneName();
    const auto *control = findContext<IR::P4Control>();
    const auto *snapTable = snapshot.table(cpName);

    const auto *entriesProp = table->properties->getProperty(IR::TableProperties::entriesPropertyName);
    const bool p4ConstEntries = entriesProp != nullptr && entriesProp->isConstant;
    const auto *defaultProp =
        table->properties->getProperty(IR::TableProperties::defaultActionPropertyName);
    const bool p4ConstDefault = defaultProp != nullptr && defaultProp->isConstant;

    auto *props = table->properties->clone();
    const auto *key = table->getKey();
    const bool keyed = key != nullptr && !key->keyElements.empty();

    if (p4ConstEntries) {
        if (snapTable != nullptr && !snapTable->rules.empty()) {
            warning(ErrorType::WARN_IGNORE,
                    "--cp-snapshot: %1% has P4 const entries; the snapshot's %2% rules are ignored",
                    cpName, snapTable->rules.size());
        }
    } else if (keyed) {
        std::vector<const CpSnapshotRule *> rules;
        if (snapTable != nullptr) {
            for (const auto &r : snapTable->rules) rules.push_back(&r);
        }
        // First match wins in the const-entry evaluator; LPM tables are re-sorted by prefix there.
        std::stable_sort(rules.begin(), rules.end(),
                         [](const auto *a, const auto *b) { return a->priority > b->priority; });
        IR::Vector<IR::Entry> entries;
        for (const auto *rule : rules) {
            IR::Vector<IR::Expression> keyset;
            for (const auto *keyElement : key->keyElements) {
                keyset.push_back(keysetOf(keyElement, findKey(*rule, keyName(keyElement)), cpName));
            }
            const auto *call = actionCallOf(table, control, *rule);
            if (call == nullptr) continue;
            entries.push_back(new IR::Entry(/*isConst=*/true, /*priority=*/nullptr,
                                            new IR::ListExpression(keyset), call,
                                            /*singleton=*/false));
        }
        // Show how small tables were materialized, in evaluation order (first match wins).
        if (!entries.empty() && entries.size() <= 8) {
            for (size_t i = 0; i < entries.size(); i++) {
                printInfo("[CP snapshot] %1% #%2%: %3% -> %4%", cpName, i,
                          entries.at(i)->keys, entries.at(i)->action);
            }
        }
        entryCount += entries.size();
        if (entries.empty()) {
            closedTables++;
        } else {
            tablesWithEntries++;
        }
        props->properties.removeByName(IR::TableProperties::entriesPropertyName);
        props->properties.push_back(new IR::Property(IR::TableProperties::entriesPropertyName,
                                                     new IR::EntriesList(entries),
                                                     /*isConstant=*/true));
    } else if (snapTable != nullptr && !snapTable->rules.empty()) {
        error(ErrorType::ERR_INVALID, "--cp-snapshot: keyless table %1% has %2% match rules",
              cpName, snapTable->rules.size());
    }

    if (defaultProp != nullptr && !p4ConstDefault) {
        const IR::Expression *defaultCall = table->getDefaultAction();
        if (snapTable != nullptr && snapTable->defaultAction.has_value()) {
            if (const auto *call = actionCallOf(table, control, *snapTable->defaultAction)) {
                defaultCall = call;
            }
        }
        props->properties.removeByName(IR::TableProperties::defaultActionPropertyName);
        props->properties.push_back(new IR::Property(IR::TableProperties::defaultActionPropertyName,
                                                     new IR::ExpressionValue(defaultCall),
                                                     /*isConstant=*/true));
    }
    table->properties = props;
    return table;
}

const IR::Node *ApplyCpSnapshot::postorder(IR::P4Program *program) {
    for (const auto &name : snapshot.unclaimedTables()) {
        warning(ErrorType::WARN_UNUSED,
                "--cp-snapshot: snapshot table %1% matches no table of the program; ignored", name);
    }
    printInfo("[CP snapshot] closed world: %1% tables with %2% snapshot entries, %3% tables empty",
              tablesWithEntries, entryCount, closedTables);
    return program;
}

/* =============================================================================================
 *  Register seeding
 * ============================================================================================= */

namespace {

/// A pinned symbolic stand-in for CP-set value @p value of type @p type.
const IR::Expression *pinnedVariable(const IR::Type *type, const std::string &name, big_int value,
                                     std::vector<const IR::Expression *> &pins) {
    const auto *var =
        ToolsVariables::getSymbolicVariable(type, cstring(std::string(kCpSnapshotVarPrefix) + name));
    pins.push_back(new IR::Equ(var, constantOf(type, value)));
    return var;
}

const IR::Expression *cellValueOf(const IR::Type *type, const std::string &name,
                                  const std::vector<big_int> &values,
                                  std::vector<const IR::Expression *> &pins) {
    if (const auto *st = type->to<IR::Type_StructLike>()) {
        IR::IndexedVector<IR::NamedExpression> fields;
        size_t i = 0;
        for (const auto *f : st->fields) {
            big_int v = i < values.size() ? values[i] : big_int(0);
            fields.push_back(new IR::NamedExpression(
                f->name, pinnedVariable(f->type, name + "_" + f->name.name.string(), v, pins)));
            i++;
        }
        return new IR::StructExpression(type, fields);
    }
    return pinnedVariable(type, name, values.empty() ? big_int(0) : values.front(), pins);
}

}  // namespace

SnapshotRegisterSeed snapshotRegisterSeed(cstring cpName, const IR::Type *indexType,
                                          const IR::Type *type) {
    SnapshotRegisterSeed seed;
    const auto *snap = loadedCpSnapshot();
    if (snap == nullptr) return seed;
    const auto *cells = snap->registerCells(cpName);
    if (cells == nullptr) return seed;
    for (const auto &[index, values] : *cells) {
        const std::string name = cpName.string() + "_" + index.str();
        seed.cells.emplace_back(constantOf(indexType, index),
                                cellValueOf(type, name, values, seed.pins));
    }
    return seed;
}

const IR::Expression *snapshotRegisterParam(cstring cpName, const IR::Type *type,
                                            std::vector<const IR::Expression *> &pins) {
    const auto *snap = loadedCpSnapshot();
    if (snap == nullptr) return nullptr;
    const auto value = snap->registerParam(cpName);
    if (!value.has_value()) return nullptr;
    return pinnedVariable(type, cpName.string(), *value, pins);
}

/* =============================================================================================
 *  Register knowledge
 * ============================================================================================= */

namespace {

bool isSnapshotVariable(const IR::SymbolicVariable *var) {
    return var->label.string_view().starts_with(kCpSnapshotVarPrefix);
}

/// Replaces every non-snapshot variable by its value in @p model, recording the snapshot
/// variables it keeps.
class FixNonSnapshotVariables : public Transform {
 public:
    FixNonSnapshotVariables(const Model &model, std::set<cstring> &kept)
        : model(model), kept(kept) {}
    const IR::Node *postorder(IR::SymbolicVariable *var) override {
        if (isSnapshotVariable(var)) {
            kept.insert(var->label);
            return var;
        }
        return model.evaluate(var, /*doComplete=*/true);
    }

 private:
    const Model &model;
    std::set<cstring> &kept;
};

/// The conjunction of @p fs's path constraint with its non-snapshot variables fixed to its model,
/// minus the snapshot pins themselves.
const IR::Expression *fixedPathCondition(const FinalState &fs, std::set<cstring> &kept) {
    const IR::Expression *out = IR::BoolLiteral::get(true);
    FixNonSnapshotVariables fix(fs.getFinalModel(), kept);
    for (const auto *constraint : fs.getExecutionState()->getPathConstraint()) {
        if (const auto *equ = constraint->to<IR::Equ>()) {
            const auto *var = equ->left->to<IR::SymbolicVariable>();
            if (var != nullptr && isSnapshotVariable(var) && equ->right->is<IR::Constant>()) {
                continue;
            }
        }
        out = new IR::LAnd(out, constraint->apply(fix));
    }
    return out;
}

}  // namespace

cstring checkRegisterKnowledge(const FinalState &phase1, const FinalState &phase2) {
    if (loadedCpSnapshot() == nullptr) return {};
    std::set<cstring> keptPhase1;
    std::set<cstring> keptPhase2;
    const IR::Expression *pc1 = nullptr;
    const IR::Expression *pc2 = nullptr;
    try {
        pc1 = fixedPathCondition(phase1, keptPhase1);
        pc2 = fixedPathCondition(phase2, keptPhase2);
    } catch (const std::exception &e) {
        printInfo("[CP snapshot] register knowledge: model substitution failed (%1%)", e.what());
        return "unknown"_cs;
    }
    if (keptPhase2.empty()) return "none"_cs;
    Z3Solver solver(/*isIncremental=*/false);
    const auto sat = solver.checkSat({pc1, new IR::LNot(pc2)});
    if (!sat.has_value()) return "unknown"_cs;
    if (!*sat) return "none"_cs;
    std::stringstream cells;
    cells << "required:";
    for (const auto &name : keptPhase2) {
        cells << " " << name.string_view().substr(kCpSnapshotVarPrefix.size());
    }
    return cstring(cells.str());
}

}  // namespace P4::P4Tools::Symbex
