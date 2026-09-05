// sha256.h ? FIPS 180-4 SHA-256, matching Node's crypto.createHash('sha256').
#ifndef gr_GR_SHA256_H
#define gr_GR_SHA256_H

#include <cstdint>
#include <string>

namespace gr {

// Hex digest (lowercase) of the byte string, like digest('hex').
std::string sha256Hex(const std::string& data);

}  // namespace gr

#endif  // gr_GR_SHA256_H
