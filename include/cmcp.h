#ifndef CMCP_H
#define CMCP_H

#define CMCP_VERSION       "0.0.0"
#define CMCP_PROTOCOL_VERSION "2025-06-18"

typedef enum {
    CMCP_OK             =  0,
    CMCP_EINVAL         = -1,
    CMCP_ENOMEM         = -2,
    CMCP_EIO            = -3,
    CMCP_EPARSE         = -4,
    CMCP_ETRANSPORT     = -5,
    CMCP_EPROTOCOL      = -6,
    CMCP_ETIMEOUT       = -7,
    CMCP_ECANCELLED     = -8,
    CMCP_EUNSUPPORTED   = -9,
    CMCP_ESCHEMA        = -10,
    CMCP_ENOTFOUND      = -11,
    CMCP_EHANDLER       = -12,
} cmcp_err_t;

const char *cmcp_errstr(int err);

#endif
