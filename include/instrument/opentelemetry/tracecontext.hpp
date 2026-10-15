#pragma once

#include <optional>

#include "http/headers.hpp"

#include "protocol.hpp"

// W3C Trace Context (https://www.w3.org/TR/trace-context/): the traceparent and tracestate headers, which carry a
// trace between services.
namespace net::instrument::opentelemetry::tracecontext
{

// parse returns the span for handling a request, as part of the trace its headers carry (as its parent span's
// child, with a new span id), or std::nullopt if they carry none (or an invalid traceparent, which is ignored, as the
// specification requires).
std::optional<span> parse(const http::headers& headers);

// inject sets the headers that carry span's trace (and span as the parent) to the next service.
void inject(const span& span, http::headers& headers);

}
