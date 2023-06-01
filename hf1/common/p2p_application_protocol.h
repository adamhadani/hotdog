#ifndef P2P_APPLICATION_PROTOCOL_
#define P2P_APPLICATION_PROTOCOL_

#include <stdint.h>
#include <packed_number.h>

#define kP2PCommandTimeSyncRequest 1
#define kP2PCommandTimeSyncReply 2

#pragma pack(push, 1)

typedef struct {
    uint8_t command;
} P2PApplicationPacketHeader;

typedef struct {
    uint64_t sync_edge_local_timestamp_ns;
} P2PTimeSyncRequestContent;

typedef struct {
    uint64_t sync_edge_local_timestamp_ns;
} P2PTimeSyncReplyContent;

#pragma pack(pop)

#endif  // P2P_APPLICATION_PROTOCOL_