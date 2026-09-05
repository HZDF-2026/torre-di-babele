// api.h — the /v1 HTTP routing layer over RoomStore.
#ifndef GR_API_H
#define GR_API_H

#include <string>

#include "http.h"
#include "store.h"

namespace gr {

// Builds the router handler for a store. Thread-safe via the store's mutex.
// token: when non-empty, every /v1 request must carry
// "Authorization: Bearer <token>" or it gets 401. The web UI shell at "/"
// is always served without auth (it prompts for the token client-side).
HttpHandler makeApiRouter(RoomStore& store, const std::string& token = "");

}  // namespace gr

#endif  // GR_API_H
