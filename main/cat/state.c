#include "cat/state.h"

const char *cat_state_name(cat_state_t st)
{
    switch (st)
    {
    case CAT_ST_DISCONNECTED:
        return "disconnected";
    case CAT_ST_PORT_OPEN:
        return "port_open";
    case CAT_ST_SYNC:
        return "sync";
    case CAT_ST_PROBE:
        return "probe";
    case CAT_ST_READY:
        return "ready";
    case CAT_ST_IQ_ENABLING:
        return "iq_enabling";
    case CAT_ST_IQ_VERIFY:
        return "iq_verify";
    case CAT_ST_IQ_ACTIVE:
        return "iq_active";
    case CAT_ST_DEGRADED:
        return "degraded";
    case CAT_ST_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}
