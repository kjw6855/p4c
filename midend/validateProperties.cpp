#include "validateProperties.h"

#include <boost/format.hpp>

#include "ir/id.h"
#include "lib/error_catalog.h"

namespace P4 {

void ValidateTableProperties::postorder(const IR::Property *property) {
    if (legalProperties.find(property->name.name) != legalProperties.end()) return;
    warn(ErrorType::WARN_IGNORE_PROPERTY, "Unknown table property: %1%", property);
}

}  // namespace P4
