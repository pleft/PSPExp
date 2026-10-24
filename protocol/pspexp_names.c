#include "pspexp_protocol.h"
#include "pspexp_opcodes.h"

const char *pspexp_channel_name(uint8_t channel)
{
    switch (channel) {
    case PSPEXP_CH_SYSTEM: return "SYSTEM";
    case PSPEXP_CH_GRAPHICS: return "GRAPHICS";
    case PSPEXP_CH_RESOURCE: return "RESOURCE";
    case PSPEXP_CH_AUDIO: return "AUDIO";
    case PSPEXP_CH_INPUT: return "INPUT";
    case PSPEXP_CH_DEBUG: return "DEBUG";
    case PSPEXP_CH_FILE: return "FILE";
    default: return "?";
    }
}

const char *pspexp_opcode_name(uint16_t opcode)
{
    switch (opcode) {
    case PSPEXP_OP_HELLO: return "HELLO";
    case PSPEXP_OP_HELLO_ACK: return "HELLO_ACK";
    case PSPEXP_OP_PRESENT_DONE: return "PRESENT_DONE";
    case PSPEXP_OP_MEMORY_QUERY: return "MEMORY_QUERY";
    case PSPEXP_OP_MEMORY_STATUS: return "MEMORY_STATUS";
    case PSPEXP_OP_CLOCK_SET: return "CLOCK_SET";
    case PSPEXP_OP_CLOCK_STATUS: return "CLOCK_STATUS";
    case PSPEXP_OP_POWER_QUERY: return "POWER_QUERY";
    case PSPEXP_OP_POWER_STATUS: return "POWER_STATUS";
    case PSPEXP_OP_ERROR: return "ERROR";
    case PSPEXP_OP_DISCONNECT: return "DISCONNECT";
    case PSPEXP_OP_BEGIN_FRAME: return "BEGIN_FRAME";
    case PSPEXP_OP_SET_CLIP: return "SET_CLIP";
    case PSPEXP_OP_DRAW_SPRITE_BATCH: return "DRAW_SPRITE_BATCH";
    case PSPEXP_OP_DRAW_TILE_BATCH: return "DRAW_TILE_BATCH";
    case PSPEXP_OP_DRAW_QUAD_BATCH: return "DRAW_QUAD_BATCH";
    case PSPEXP_OP_DRAW_RECT: return "DRAW_RECT";
    case PSPEXP_OP_PRESENT: return "PRESENT";
    case PSPEXP_OP_DRAW_LINE: return "DRAW_LINE";
    case PSPEXP_OP_3D_STATE: return "3D_STATE";
    case PSPEXP_OP_3D_MATRICES: return "3D_MATRICES";
    case PSPEXP_OP_3D_LIGHTS: return "3D_LIGHTS";
    case PSPEXP_OP_3D_DRAW: return "3D_DRAW";
    case PSPEXP_OP_3D_CLEAR: return "3D_CLEAR";
    case PSPEXP_OP_3D_DRAW_MESH: return "3D_DRAW_MESH";
    case PSPEXP_OP_3D_TARGET: return "3D_TARGET";
    case PSPEXP_OP_3D_MORPH: return "3D_MORPH";
    case PSPEXP_OP_3D_COPY: return "3D_COPY";
    case PSPEXP_OP_RESOURCE_CREATE_TARGET: return "RESOURCE_CREATE_TARGET";
    case PSPEXP_OP_RESOURCE_PATCH: return "RESOURCE_PATCH";
    case PSPEXP_OP_RESOURCE_UPLOAD_MESH: return "RESOURCE_UPLOAD_MESH";
    case PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE: return "RESOURCE_UPLOAD_TEXTURE";
    case PSPEXP_OP_RESOURCE_RELEASE: return "RESOURCE_RELEASE";
    case PSPEXP_OP_RESOURCE_HINT: return "RESOURCE_HINT";
    case PSPEXP_OP_RESOURCE_READY: return "RESOURCE_READY";
    case PSPEXP_OP_RESOURCE_ERROR: return "RESOURCE_ERROR";
    case PSPEXP_OP_RESOURCE_TIER: return "RESOURCE_TIER";
    case PSPEXP_OP_RESOURCE_MISS: return "RESOURCE_MISS";
    case PSPEXP_OP_RESOURCE_UPLOAD_PALETTE: return "RESOURCE_UPLOAD_PALETTE";
    case PSPEXP_OP_INPUT_SAMPLE: return "INPUT_SAMPLE";
    case PSPEXP_OP_RESOURCE_UPLOAD_SOUND: return "RESOURCE_UPLOAD_SOUND";
    case PSPEXP_OP_PLAY_SOUND: return "PLAY_SOUND";
    case PSPEXP_OP_STOP_SOUND: return "STOP_SOUND";
    case PSPEXP_OP_AUDIO_STREAM: return "AUDIO_STREAM";
    case PSPEXP_OP_AUDIO_STATUS: return "AUDIO_STATUS";
    case PSPEXP_OP_AUDIO_STREAM_RESET: return "AUDIO_STREAM_RESET";
    case PSPEXP_OP_AUDIO_CODED_STREAM: return "AUDIO_CODED_STREAM";
    case PSPEXP_OP_OSD_VOLUME: return "OSD_VOLUME";
    case PSPEXP_OP_RESTART: return "RESTART";
    case PSPEXP_OP_BOOT_DISC: return "BOOT_DISC";
    case PSPEXP_OP_CAPTURE_SET: return "CAPTURE_SET";
    case PSPEXP_OP_CAPTURE_DATA: return "CAPTURE_DATA";
    case PSPEXP_OP_TEXT_REQUEST: return "TEXT_REQUEST";
    case PSPEXP_OP_TEXT_RESULT: return "TEXT_RESULT";
    case PSPEXP_OP_BENCH_START: return "BENCH_START";
    case PSPEXP_OP_BENCH_DATA: return "BENCH_DATA";
    case PSPEXP_OP_BENCH_END: return "BENCH_END";
    case PSPEXP_OP_BENCH_REPORT: return "BENCH_REPORT";
    case PSPEXP_OP_BENCH_ECHO: return "BENCH_ECHO";
    case PSPEXP_OP_BENCH_UPLINK: return "BENCH_UPLINK";
    case PSPEXP_OP_DEBUG_SCREENSHOT: return "DEBUG_SCREENSHOT";
    case PSPEXP_OP_FS_LIST: return "FS_LIST";
    case PSPEXP_OP_FS_READ: return "FS_READ";
    case PSPEXP_OP_FS_WRITE: return "FS_WRITE";
    case PSPEXP_OP_FS_MKDIR: return "FS_MKDIR";
    case PSPEXP_OP_FS_REMOVE: return "FS_REMOVE";
    case PSPEXP_OP_FS_RENAME: return "FS_RENAME";
    case PSPEXP_OP_FS_INFO: return "FS_INFO";
    case PSPEXP_OP_FS_STAT: return "FS_STAT";
    case PSPEXP_OP_FS_REPLY: return "FS_REPLY";
    default: return "?";
    }
}

const char *pspexp_resource_error_name(uint16_t reason)
{
    switch (reason) {
    case PSPEXP_RERR_NONE: return "NONE";
    case PSPEXP_RERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case PSPEXP_RERR_BAD_FORMAT: return "BAD_FORMAT";
    case PSPEXP_RERR_BAD_DIMENSIONS: return "BAD_DIMENSIONS";
    case PSPEXP_RERR_BAD_LENGTH: return "BAD_LENGTH";
    case PSPEXP_RERR_BAD_LAYOUT: return "BAD_LAYOUT";
    case PSPEXP_RERR_BAD_FRAME_TABLE: return "BAD_FRAME_TABLE";
    case PSPEXP_RERR_DUPLICATE_ID: return "DUPLICATE_ID";
    case PSPEXP_RERR_UNKNOWN_ID: return "UNKNOWN_ID";
    case PSPEXP_RERR_TABLE_FULL: return "TABLE_FULL";
    case PSPEXP_RERR_BAD_PALETTE: return "BAD_PALETTE";
    case PSPEXP_RERR_WRONG_KIND: return "WRONG_KIND";
    default: return "?";
    }
}
