// mcp.h — greenroom as an MCP stdio server (JSON-RPC 2.0, line-delimited).
#ifndef GR_MCP_H
#define GR_MCP_H

#include <string>

namespace gr {

// Blocks: reads JSON-RPC lines from stdin, writes responses to stdout.
// Proxies to the greenroom serve process at GREENROOM_URL.
int runMcp();

}  // namespace gr

#endif  // GR_MCP_H
