#pragma once

#include <optional>

#include "http/headers.hpp"

#include "protocol.hpp"

namespace net::instrument::opentelemetry::tracecontext
{

std::optional<span> parse(const http::headers& headers) noexcept;
void                inject(http::headers& headers) noexcept;

}
