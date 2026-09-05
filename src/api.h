// api.h — the /v1 HTTP routing layer over RoomStore.
#ifndef GR_API_H
#define GR_API_H

#include "http.h"
#include "store.h"

namespace gr {

// Builds the router handler for a store. Thread-safe via the store's mutex.
HttpHandler makeApiRouter(RoomStore& store);

}  // namespace gr

#endif  // GR_API_H
