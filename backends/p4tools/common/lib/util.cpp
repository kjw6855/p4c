#include "backends/p4tools/common/lib/util.h"

#include <chrono>  // NOLINT cpplint throws a warning because Google has a similar library...
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <numeric>
#include <optional>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/cpp_int/add.hpp>
#include <boost/multiprecision/detail/et_ops.hpp>
#include <boost/multiprecision/number.hpp>
#include <boost/random/uniform_int_distribution.hpp>

#include "ir/id.h"
#include "ir/irutils.h"
#include "ir/vector.h"
#include "lib/exceptions.h"
#include "lib/null.h"
#include "frontends/p4/optimizeExpressions.h"

namespace P4::P4Tools {

/* =========================================================================================
 *  Seeds, timestamps, randomness.
 * ========================================================================================= */

std::optional<uint32_t> Utils::currentSeed = std::nullopt;

boost::random::mt19937 Utils::rng(0);

std::string Utils::getTimeStamp() {
    // get current time
    auto now = std::chrono::system_clock::now();
    // get number of milliseconds for the current second
    // (remainder after division into seconds)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    // convert to std::time_t in order to convert to std::tm (broken time)
    auto timer = std::chrono::system_clock::to_time_t(now);
    // convert to broken time
    std::tm *bt = std::localtime(&timer);
    CHECK_NULL(bt);
    std::stringstream oss;
    oss << std::put_time(bt, "%Y-%m-%d-%H:%M:%S");  // HH:MM:SS
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void Utils::setRandomSeed(int seed) {
    if (currentSeed.has_value()) {
        BUG("Seed already initialized with %1%.", currentSeed.value());
    }
    currentSeed = seed;
    rng.seed(seed);
}

std::optional<uint32_t> Utils::getCurrentSeed() { return currentSeed; }

uint64_t Utils::getRandInt(uint64_t max) {
    if (!currentSeed) {
        return 0;
    }
    boost::random::uniform_int_distribution<uint64_t> dist(0, max);
    return dist(rng);
}

int64_t Utils::getRandInt(int64_t min, int64_t max) {
    boost::random::uniform_int_distribution<int64_t> distribution(min, max);
    return distribution(rng);
}

int64_t Utils::getRandInt(const std::vector<int64_t> &percent) {
    int sum = std::accumulate(percent.begin(), percent.end(), 0);

    // Do not pick zero since that conflicts with zero percentage values.
    auto randNum = getRandInt(1, sum);
    int ret = 0;

    int64_t retSum = 0;
    for (auto i : percent) {
        retSum += i;
        if (retSum >= randNum) {
            break;
        }
        ret = ret + 1;
    }
    return ret;
}

big_int Utils::getRandBigInt(const big_int &max) {
    if (!currentSeed) {
        return 0;
    }
    boost::random::uniform_int_distribution<big_int> dist(0, max);
    return dist(rng);
}

big_int Utils::getRandBigInt(const big_int &min, const big_int &max) {
    if (!currentSeed) {
        return 0;
    }
    boost::random::uniform_int_distribution<big_int> dist(min, max);
    return dist(rng);
}

const IR::Constant *Utils::getRandConstantForWidth(int bitWidth) {
    auto maxVal = IR::getMaxBvVal(bitWidth);
    auto randInt = Utils::getRandBigInt(maxVal);
    const auto *constType = IR::Type_Bits::get(bitWidth);
    return IR::Constant::get(constType, randInt);
}

const IR::Constant *Utils::getRandConstantForType(const IR::Type_Bits *type) {
    auto maxVal = IR::getMaxBvVal(type->width_bits());
    auto randInt = Utils::getRandBigInt(maxVal);
    return IR::Constant::get(type, randInt);
}

const IR::Expression *Utils::getValExpr(const std::string &strVal, size_t bitWidth) {
    const auto *baseVar = P4::optimizeExpression(IR::Constant::get(IR::Type_Bits::get(0), 0));
    const auto *baseVarType = IR::Type_Bits::get(bitWidth);

    int baseLen = (static_cast<int>(bitWidth) - 1) / 8 + 1;
    int valLen = std::min(baseLen, static_cast<int>(strVal.length()));

    for (size_t w = 0; w < bitWidth; w += 32) {
        int num = 0;
        int subBitWidth = std::min(32, static_cast<int>(bitWidth) - static_cast<int>(w));
        int shl = (subBitWidth - 1) / 8;
        for (int i = 0; i < subBitWidth; i += 8) {
            int baseIdx = (static_cast<int>(i) + static_cast<int>(w)) / 8;
            int idx = baseIdx - baseLen + valLen;
            if (idx < 0) continue;
            num |= static_cast<int>(static_cast<unsigned char>(strVal[idx]) << (shl * 8 - i));
        }
        const auto *concat = new IR::Concat(
            baseVarType, baseVar,
            IR::Constant::get(IR::Type_Bits::get(subBitWidth), static_cast<unsigned int>(num)));
        baseVar = P4::optimizeExpression(concat);
    }

    return baseVar;
}

big_int Utils::getVal(const std::string &strVal, size_t bitWidth) {
    if (strVal.length() * 8 > bitWidth) bitWidth = strVal.length() * 8;
    const auto *valExpr = Utils::getValExpr(strVal, bitWidth);
    BUG_CHECK(valExpr->is<IR::Constant>(), "getVal: expression is not a constant");
    return valExpr->checkedTo<IR::Constant>()->value;
}

const IR::Expression *Utils::removeUnknownVar(const IR::Expression *expr) {
    if (const auto *symVar = expr->to<IR::SymbolicVariable>()) {
        if (symVar->label.startsWith("pktVar")) return nullptr;
        return expr;
    }
    if (const auto *binary = expr->to<IR::Operation_Binary>()) {
        if (binary->is<IR::ArrayIndex>()) return removeUnknownVar(binary->right);
        const auto *leftExpr = removeUnknownVar(binary->left);
        const auto *rightExpr = removeUnknownVar(binary->right);
        if (leftExpr != nullptr) {
            if (rightExpr != nullptr) {
                auto bitWidth =
                    leftExpr->type->width_bits() + rightExpr->type->width_bits();
                return P4::optimizeExpression(
                    new IR::Concat(IR::Type_Bits::get(bitWidth), leftExpr, rightExpr));
            }
            return leftExpr;
        }
        return rightExpr;
    }
    return expr;
}

const IR::Constant *Utils::getZeroCksum(const IR::Expression *expr, int zeroLen, bool init) {
    if (const auto *symVar = expr->to<IR::SymbolicVariable>()) {
        if (symVar->label.startsWith("*method_checksum")) {
            if (init) return IR::Constant::get(IR::Type_Bits::get(8), 0);
            if (zeroLen < 64) return IR::Constant::get(IR::Type_Bits::get(16), 0);
        }
        return nullptr;
    }
    if (const auto *constVal = expr->to<IR::Constant>()) {
        if (constVal->value == 0) {
            auto bitWidth = constVal->type->width_bits() + zeroLen;
            return IR::Constant::get(IR::Type_Bits::get(bitWidth), 1);
        }
        return nullptr;
    }
    if (const auto *binary = expr->to<IR::Operation_Binary>()) {
        if (binary->is<IR::ArrayIndex>()) return getZeroCksum(binary->right, zeroLen, init);
        auto *retVal = getZeroCksum(binary->right, zeroLen, init);
        if (retVal == nullptr) return nullptr;
        if (retVal->value == 0) return retVal;
        return getZeroCksum(binary->left, retVal->type->width_bits(), false);
    }
    return nullptr;
}

bool Utils::isDefaultByConstraint(const IR::Expression *constraint) {
    if (constraint->is<IR::Neq>()) return true;
    if (const auto *expr = constraint->to<IR::LAnd>()) {
        return Utils::isDefaultByConstraint(expr->left) &&
               Utils::isDefaultByConstraint(expr->right);
    }
    if (const auto *expr = constraint->to<IR::LOr>()) {
        return Utils::isDefaultByConstraint(expr->left) ||
               Utils::isDefaultByConstraint(expr->right);
    }
    return false;
}

std::optional<bool> Utils::evalCondWithTaint(const IR::Expression *cond) {
    if (cond->is<IR::Neq>()) {
        return false;
    }
    if (const auto *val = cond->to<IR::BoolLiteral>()) {
        return val->value;
    }
    if (const auto *expr = cond->to<IR::LAnd>()) {
        auto leftCond = Utils::evalCondWithTaint(expr->left);
        auto rightCond = Utils::evalCondWithTaint(expr->right);
        if (leftCond.has_value() && !leftCond.value()) return false;
        if (rightCond.has_value() && !rightCond.value()) return false;
        return std::nullopt;
    }
    if (const auto *expr = cond->to<IR::LOr>()) {
        auto leftCond = Utils::evalCondWithTaint(expr->left);
        auto rightCond = Utils::evalCondWithTaint(expr->right);
        if (leftCond.has_value() && leftCond.value()) return true;
        if (rightCond.has_value() && rightCond.value()) return true;
        return std::nullopt;
    }
    return std::nullopt;
}

/* =========================================================================================
 *  Other.
 * ========================================================================================= */

const IR::MethodCallExpression *Utils::generateInternalMethodCall(
    std::string_view methodName, const std::vector<const IR::Expression *> &argVector,
    const IR::Type *returnType, const IR::ParameterList *paramList) {
    auto *args = new IR::Vector<IR::Argument>();
    for (const auto *expr : argVector) {
        args->push_back(new IR::Argument(expr));
    }
    cstring name(methodName);
    return new IR::MethodCallExpression(
        returnType,
        new IR::Member(new IR::Type_Method(paramList, name),
                       new IR::PathExpression(new IR::Type_Extern("*"), new IR::Path("*")), name),
        args);
}

std::vector<const IR::Type_Declaration *> argumentsToTypeDeclarations(
    const IR::IGeneralNamespace *ns, const IR::Vector<IR::Argument> *inputArgs) {
    std::vector<const IR::Type_Declaration *> resultDecls;
    for (const auto *arg : *inputArgs) {
        const auto *expr = arg->expression;

        const IR::Type_Declaration *declType = nullptr;

        if (const auto *ctorCall = expr->to<IR::ConstructorCallExpression>()) {
            const auto *constructedTypeName = ctorCall->constructedType->checkedTo<IR::Type_Name>();
            // Find the corresponding type declaration in the top-level namespace.
            declType =
                findProgramDecl(ns, constructedTypeName->path)->checkedTo<IR::Type_Declaration>();
        } else if (const auto *pathExpr = expr->to<IR::PathExpression>()) {
            // Look up the path expression in the top-level namespace and expect to find a
            // declaration instance.
            const auto *declInstance =
                findProgramDecl(ns, pathExpr->path)->checkedTo<IR::Declaration_Instance>();
            declType = declInstance->type->checkedTo<IR::Type_Declaration>();
        } else {
            BUG("Unexpected main-declaration argument node type: %1%", expr->node_type_name());
        }

        // The constructor's parameter list should be empty, since the compiler should have
        // substituted the constructor arguments for us.
        if (const auto *iApply = declType->to<IR::IContainer>()) {
            const IR::ParameterList *ctorParams = iApply->getConstructorParameters();
            BUG_CHECK(ctorParams->empty(), "Compiler did not eliminate constructor parameters: %1%",
                      ctorParams);
        } else {
            BUG("Does not instantiate an IContainer: %1%", expr);
        }

        resultDecls.emplace_back(declType);
    }
    return resultDecls;
}

const IR::IDeclaration *findProgramDecl(const IR::IGeneralNamespace *ns, const IR::Path *path) {
    auto name = path->name.name;
    const auto *decl = ns->getDeclsByName(name)->singleOrDefault();
    if (decl != nullptr) {
        return decl;
    }
    BUG("Variable %1% not found in the available namespaces.", path);
}

const IR::IDeclaration *findProgramDecl(const IR::IGeneralNamespace *ns,
                                        const IR::PathExpression *pathExpr) {
    return findProgramDecl(ns, pathExpr->path);
}

const IR::Type_Declaration *resolveProgramType(const IR::IGeneralNamespace *ns,
                                               const IR::Type_Name *type) {
    const auto *path = type->path;
    const auto *decl = findProgramDecl(ns, path)->to<IR::Type_Declaration>();
    BUG_CHECK(decl, "Not a type: %1%", path);
    return decl;
}

}  // namespace P4::P4Tools
