// integrity.h — binary self-integrity (PROTOCOL.md §Protection).
#ifndef gr_GR_PROTECT_INTEGRITY_H
#define gr_GR_PROTECT_INTEGRITY_H

#include <string>

namespace gr {

// SHA-256 hex digest of the running executable file itself. Empty string if
// the image path cannot be resolved or read. Users compare this against the
// digest published with the release: `babele selfhash` prints it.
std::string selfSha256();

}  // namespace gr

#endif  // gr_GR_PROTECT_INTEGRITY_H
