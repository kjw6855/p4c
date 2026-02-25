#ifndef BACKENDS_STATE_DEPENDENCY_OPTIONS_H_
#define BACKENDS_STATE_DEPENDENCY_OPTIONS_H_

#include <filesystem>
#include "frontends/common/options.h"

namespace P4::P4StateDependency {

class P4StateDependencyOptions : public CompilerOptions {
 public:
    P4StateDependencyOptions();
    virtual ~P4StateDependencyOptions() = default;

    std::filesystem::path graphsDir{"."};
    bool loadIRFromJson = false;  // read from json
    bool graphs = true;           // default behavior
    bool fullGraph = false;
    bool jsonOut = false;
    bool showVar = false;
    bool setActionAsProc = false;

 private:
    bool isGraphsSet = false;
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_OPTIONS_H_ */
