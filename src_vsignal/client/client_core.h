/**
 * @file client_core.h
 * @brief vsignal client core — thin wrapper over shared tw::client.
 */
#ifndef VSIGNAL_CLIENT_CORE_H
#define VSIGNAL_CLIENT_CORE_H

#include "tw/client.h"

namespace vsignal {
namespace client {

    using tw::client::send_request;
    using tw::client::build_request;
    using tw::client::print_response;

}  // namespace client
}  // namespace vsignal

#endif  // VSIGNAL_CLIENT_CORE_H
