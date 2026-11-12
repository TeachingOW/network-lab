// HTTP/1.1 request framing, routing and response serialisation.
#pragma once

#include "conn.h"

// Parses every complete request buffered in c.in, runs its handler and appends
// the responses to c.out (pipelining-aware). Leaves a partial request in c.in
// and records in c.need_total how many bytes it needs.
void process(Conn& c, Worker& w);

// Appends the constant "408 Request Timeout" response (with Connection: close).
void append_timeout_response(std::string& out);
