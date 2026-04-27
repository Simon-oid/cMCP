#include "cmcp.h"

const char *cmcp_errstr(int err) {
    switch (err) {
    case CMCP_OK:           return "ok";
    case CMCP_EINVAL:       return "invalid argument";
    case CMCP_ENOMEM:       return "out of memory";
    case CMCP_EIO:          return "I/O error";
    case CMCP_EPARSE:       return "parse error";
    case CMCP_ETRANSPORT:   return "transport error";
    case CMCP_EPROTOCOL:    return "protocol error";
    case CMCP_ETIMEOUT:     return "timeout";
    case CMCP_ECANCELLED:   return "cancelled";
    case CMCP_EUNSUPPORTED: return "unsupported";
    case CMCP_ESCHEMA:      return "schema validation failed";
    case CMCP_ENOTFOUND:    return "not found";
    case CMCP_EHANDLER:     return "handler error";
    default:                return "unknown error";
    }
}
