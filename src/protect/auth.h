// auth.h — agent key verification as BVM bytecode (see vm.h).
#ifndef gr_GR_PROTECT_AUTH_H
#define gr_GR_PROTECT_AUTH_H

#include <string>
#include <utility>
#include <vector>

namespace gr {

// true iff (name, key) authenticates against the registry. Each registry
// entry is (agentName, sha256Hex(key)). The name scan, the digest comparison
// and the verdict all execute inside the VM; this host function only
// serializes the registry into the VM data buffer. Fail closed on any fault.
bool bvmAgentCheck(const std::vector<std::pair<std::string, std::string>>& registry,
                   const std::string& name, const std::string& key);

}  // namespace gr

#endif  // gr_GR_PROTECT_AUTH_H
