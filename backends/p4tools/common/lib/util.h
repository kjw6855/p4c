#ifndef BACKENDS_P4TOOLS_COMMON_LIB_UTIL_H_
#define BACKENDS_P4TOOLS_COMMON_LIB_UTIL_H_

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <boost/random/mersenne_twister.hpp>

#include "ir/ir.h"

namespace P4::P4Tools {

/// General utility functions that are not present in the compiler framework.
class Utils {
    /* =========================================================================================
     *  Seeds, timestamps, randomness.
     * ========================================================================================= */
 private:
    /// The random generator of this project. It is initialized with the input seed.
    static boost::random::mt19937 rng;

    /// Stores the state of the PRNG.
    static std::optional<uint32_t> currentSeed;

 public:
    /// Return the current timestamp with millisecond accuracy.
    /// Format: year-month-day-hour:minute:second.millisecond
    /// Borrowed from https://stackoverflow.com/a/35157784
    static std::string getTimeStamp();

    /// Initialize the random generator with an integer seed. This also seeds @var currentSeed.
    /// Uses boost's mersenne twister.
    static void setRandomSeed(int seed);

    /// @returns currentSeed.
    static std::optional<uint32_t> getCurrentSeed();

    /// @returns a random integer in the range [0, @param max]. Always return 0 if no seed is set.
    static uint64_t getRandInt(uint64_t max);

    /// @returns a random integer between min and max.
    static int64_t getRandInt(int64_t min, int64_t max);

    /// @returns a random integer based on the percent vector.
    static int64_t getRandInt(const std::vector<int64_t> &percent);

    /// @returns a random big integer in the range [0, @param max]. Always return 0 if no seed is
    /// set.
    static big_int getRandBigInt(const big_int &max);

    /// This is a big_int version of getRndInt.
    static big_int getRandBigInt(const big_int &min, const big_int &max);

    /// @returns a IR::Constant with a random big integer that fits the specified bit width.
    /// The type will be an unsigned Type_Bits with @param bitWidth.
    static const IR::Constant *getRandConstantForWidth(int bitWidth);

    /// @returns a IR::Constant with a random big integer that fits the specified @param type.
    static const IR::Constant *getRandConstantForType(const IR::Type_Bits *type);

    /// Converts a raw byte string @param strVal of @param strValBitLen bits into an IR expression
    /// by assembling up to 32-bit chunks via Concat nodes and folding with optimizeExpression.
    static const IR::Expression *getValExpr(const std::string &strVal, size_t strValBitLen);

    /// @returns the big_int value represented by the raw byte string @param strVal interpreted as
    /// a @param bitWidth-bit unsigned integer.
    static big_int getVal(const std::string &strVal, size_t bitWidth);

    /// Removes symbolic packet variables (those with label starting with "pktVar") from an
    /// expression tree, concatenating the remaining sub-expressions. Returns nullptr if the
    /// entire expression is unknown.
    static const IR::Expression *removeUnknownVar(const IR::Expression *expr);

    /// Traverses @param expr to find a checksum symbolic variable (label starting with
    /// "*method_checksum"). Returns a zero-valued constant indicating checksum position
    /// metadata, or nullptr if none is found.
    static const IR::Constant *getZeroCksum(const IR::Expression *expr, int zeroLen, bool init);

    /// @returns true if @param constraint is composed entirely of Neq nodes (combined with
    /// LAnd/LOr), which indicates the constraint encodes a default (wildcard) match.
    static bool isDefaultByConstraint(const IR::Expression *constraint);

    /// Evaluates a boolean condition that may contain tainted (symbolic/unknown) sub-expressions.
    /// Returns true/false if the result can be determined despite taint, or std::nullopt if the
    /// outcome is unknowable (tainted). Handles LAnd, LOr, BoolLiteral, and Neq nodes.
    static std::optional<bool> evalCondWithTaint(const IR::Expression *cond);

    /* =========================================================================================
     *  Other.
     * ========================================================================================= */
 public:
    /// @returns a method call to an internal extern consumed by the interpreter. The return type
    /// is typically Type_Void.
    static const IR::MethodCallExpression *generateInternalMethodCall(
        std::string_view methodName, const std::vector<const IR::Expression *> &argVector,
        const IR::Type *returnType = IR::Type_Void::get(),
        const IR::ParameterList *paramList = new IR::ParameterList());

    /// Shuffles the given iterable @param inp
    template <typename T>
    static void shuffle(T *inp) {
        std::shuffle(inp->begin(), inp->end(), rng);
    }

    /// @returns a random element from the given range between @param start and @param end.
    template <typename Iter>
    static Iter pickRandom(Iter start, Iter end) {
        int random = getRandInt(std::distance(start, end) - 1);
        std::advance(start, random);
        return start;
    }

    /// Convert a container type (array, set, vector, etc.) into a well-formed [val1, val2, ...]
    /// representation. This function is used for debugging output.
    template <typename ContainerType>
    static std::string containerToString(const ContainerType &container) {
        std::stringstream stringStream;

        stringStream << '[';
        auto val = container.cbegin();
        if (val != container.cend()) {
            stringStream << *val++;
            while (val != container.cend()) {
                stringStream << ", " << *val++;
            }
        }
        stringStream << ']';
        return stringStream.str();
    }
};

/// Converts the list of arguments @inputArgs to a list of type declarations. Any names appearing in
/// the arguments are resolved with @ns.
/// This is mainly useful for inspecting package instances.
std::vector<const IR::Type_Declaration *> argumentsToTypeDeclarations(
    const IR::IGeneralNamespace *ns, const IR::Vector<IR::Argument> *inputArgs);

/// Looks up a declaration from a path. A BUG occurs if no declaration is found.
const IR::IDeclaration *findProgramDecl(const IR::IGeneralNamespace *ns, const IR::Path *path);

/// Looks up a declaration from a path expression. A BUG occurs if no declaration is found.
const IR::IDeclaration *findProgramDecl(const IR::IGeneralNamespace *ns,
                                        const IR::PathExpression *pathExpr);

/// Resolves a Type_Name in the top-level namespace.
const IR::Type_Declaration *resolveProgramType(const IR::IGeneralNamespace *ns,
                                               const IR::Type_Name *type);

}  // namespace P4::P4Tools

#endif /* BACKENDS_P4TOOLS_COMMON_LIB_UTIL_H_ */
