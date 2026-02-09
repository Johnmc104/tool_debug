/**
 * @file client_core.h
 * @brief vwave client core — thin wrapper over shared tw::client.
 *
 * Provides wave::client namespace that delegates to tw::client.
 */
#ifndef WAVE_CLIENT_CORE_H
#define WAVE_CLIENT_CORE_H

#include "tw/client.h"

namespace wave {
namespace client {

    using tw::client::send_request;
    using tw::client::build_request;
    using tw::client::read_signal_file;
    using tw::client::print_response;

}  // namespace client
}  // namespace wave

#endif  // WAVE_CLIENT_CORE_H
