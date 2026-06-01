#pragma once

// NotifySerialization — single source of truth for converting NotifyTrack data
// to/from a JSON text representation.
//
// Used by BOTH:
//   * ComponentSerializers (scene/world .iscn serialization of TimelineComponent)
//   * AnimationSerializer  (.ianim asset embedded notify section — Unreal-style
//                           notifies that live on the animation clip itself)
//
// Keeping one schema here means a .ianim notify section and an .iscn
// TimelineComponent are byte-for-byte interchangeable, and the editor can move
// notify data between an entity timeline and a clip asset without re-mapping.
//
// The JSON document shape (matches the historical ComponentSerializers layout
// so old scene files keep loading):
//   {
//     "dur": <float clipDuration>,
//     "nid": <uint nextNotifyId>,
//     "trk": [ { "nm", "cat", "mu", "n":[ <notify> ], "s":[ <state> ] } ]
//   }
//   notify = { "id","cat","t","nm","col","pb": { key: {"t","v"} } }
//   state  = { "id","cat","t0","t1","nm","col","pb": { ... } }
//   pb value: { "t": "i"|"f"|"s"|"v3"|"v4", "v": <payload> }

#include "ECS/NotifyTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NotifyIO
{
    // Serialize a set of notify tracks (+ duration + next-id allocator) to a
    // compact JSON string. Never throws.
    std::string TracksToJsonString(const std::vector<NotifyTrack>& tracks,
                                   float                           clipDuration,
                                   uint32_t                        nextNotifyId);

    // Parse a JSON string produced by TracksToJsonString. Out-params may be
    // null if the caller doesn't care about that field. Returns false on a
    // parse error (out-params left untouched on failure). An empty input
    // string yields an empty track list and returns true.
    bool TracksFromJsonString(const std::string&        json,
                              std::vector<NotifyTrack>& outTracks,
                              float*                    outClipDuration = nullptr,
                              uint32_t*                 outNextNotifyId = nullptr);
}
