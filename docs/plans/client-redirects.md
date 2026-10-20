# Plan: following redirects in the HTTP client

Status: proposed (2026-10-19). The questions at the end are open.

## Goal

`http::client` follows redirects (301, 302, 303, 307 and 308) as Go's client and browsers do, so a side project
calling an API or downloading a file gets the resource rather than a 3xx it has to handle itself; without sending
credentials to a host they weren't meant for, and without changing the URLs it's sent to (many redirects go to signed
CDN or object-store URLs, whose signatures cover the path and query as they're written).

## Where things stand

- `client::send(client_request)` returns the first response, 3xx included. Nothing reads `Location`.
- `client_request::uri` is a `url`, which keeps its parts **decoded**: `path` a decoded string, `query` an
  `unordered_map` of decoded names and values (`url::parse_query()`, reading `+` as a space). The request line is
  `url::origin_form()`, which re-encodes them: the path's `%2F` goes out as `/` (another path), the query's parameters
  are sorted by name, and a `+` goes out as `%20`. So the client doesn't send a URL as it was given. A redirect to e.g.
  `...?Expires=1&Signature=ab+c/d=` (CloudFront's form) would go out as `...?Expires=1&Signature=ab%20c%2Fd%3D`, which
  the CDN rejects; `get()` with such a URL fails the same way today.
- `url::parse()` reads a string with neither a scheme nor `//`, not starting with `/`, as starting with its host
  (`example.com/path`): convenient for `get("example.com")`, but not how a relative reference (`Location: next/page`)
  reads. There's no reference resolution (RFC 3986 section 5).
- A request's body is a `std::unique_ptr<io::reader>`, read once while sending: a redirect that must send it again
  (307, 308) has nothing to read. `set_body(std::string)` makes an `io::owned_string_reader`, whose string could be read
  again.
- `client_response` has no URL: after redirects, nothing would say where the response came from.
- The client already sends some requests again (a reused connection found closed, HTTP/2's refused streams): those are
  retries of the same request, inside `send()`; a redirect is a new request, so it goes around them.
- Errors are `std::error_code`s; the HTTP client has no error enum of its own (`route_error` is the router's).

## Model: Go's `net/http` client

- Follows 301, 302, 303, 307 and 308 (not 300, 304, 305). A 3xx without `Location` is returned as it is.
- 301, 302 and 303 become GET without a body (HEAD stays HEAD; GET stays GET), as browsers do: RFC 9110 (15.4.2/3)
  allows POST to become GET for 301/302, and 303 means "GET this instead". 307 and 308 keep the method and the body,
  which needs `Request.GetBody` (set automatically for in-memory bodies): without it, the 307/308 is returned.
- At most 10 redirects (`CheckRedirect`, a per-client callback, decides; its default stops at 10 with an error; it
  may return `ErrUseLastResponse` to get the 3xx itself).
- Headers are copied to the next request, but `Authorization`, `WWW-Authenticate`, `Cookie` and `Cookie2` only to the
  same host or its subdomains (`shouldCopyHeaderOnRedirect`); once dropped, they stay dropped for later hops (since
  Go 1.23.5, CVE-2024-45336). It sets `Referer` to the previous URL, except from https to http.
- `Location` is resolved against the request's URL (RFC 3986 5.2); the fragment is kept when `Location` has none (RFC
  9110 10.2.2).
- The previous response's body is read and discarded if it's small (up to 2 KiB), so its connection is reused, then
  closed.
- `Client.Timeout` covers every hop. cppnet has no overall timeout (its timeouts are per step), only the stop token.

Others: curl follows only with `-L` (50 at most), and since 7.83 drops credentials if the host, port or scheme
changes; reqwest, OkHttp and browsers follow by default (10, 20, 20).

## Design

### URLs as given (stage 1)

`url` keeps its path and query **encoded**, as given: `parse()` keeps valid percent-encodings as they are (`%2F` stays
`%2F`; `%2f` too), and encodes only what must be (spaces, non-ASCII, controls); `query` becomes a `std::string` (without
the `?`), and `url::parse_query(u.query)` (which exists) gives the values; `origin_form()` is then the path and query
joined, nothing re-encoded. Code building a URL from parts encodes them (`url::encode()`, which exists). Go's `url.URL`
keeps a decoded `Path` with a `RawPath` hint and a raw `RawQuery`; keeping both forms is how Go gets bugs where they
disagree, so this keeps one (Open question 1).

`url::resolve(base, reference)` resolves a reference string against a URL (RFC 3986 5.2, with 5.2.4's dot-segment
removal), reading the reference strictly (`next/page` is a path; no host-first shortcut), and fails with
`std::errc::invalid_argument` for one that isn't a valid reference. Its tests are RFC 3986 5.4's examples (normal and
abnormal), plus encodings kept as they are; `fuzz_url` also checks that resolving against a URL and re-parsing what it
builds agree.

### Following (stage 2)

`send()` becomes a loop around what it does now (each hop with its own retries, timeouts and connection):

1. Send the request; if the response isn't 301, 302, 303, 307 or 308, has no `Location`, or the request's limit is 0,
   return it.
2. If this would be one more than the limit, fail with `http::client_errc::too_many_redirects` (Open question 4).
3. Resolve `Location` against the request's URL (a bad one fails with `client_errc::bad_redirect`, as Go fails); a
   scheme other than `http`/`https` fails with `std::errc::protocol_not_supported`, as a request to it would. The
   fragment is kept when `Location` has none.
4. The next request: 301/302 (a method other than GET or HEAD) and 303 (other than HEAD) become GET without a body,
   dropping the body's fields (RFC 9110 15.4: `Content-Length`, `Content-Type`, `Content-Encoding`,
   `Content-Language`, `Content-Location`, `Transfer-Encoding`); 307/308 keep the method and need the body again:
   `client_request::get_body` (Open question 5); without one, the 307/308 is returned as it is. Credentials are kept or
   dropped (Open question 6). A `Host` the caller set is dropped when the host changes.
5. Discard the previous response: its body read to its end if what's left is at most 2 KiB (as Go), so its HTTP/1.x
   connection is reused; otherwise it's closed (HTTP/2: the stream reset). Then loop.

The stop token covers every hop (it's the request's); each hop has the client's timeouts. The response returned carries
its URL (Open question 8).

### Configuration

`client_options::max_redirects` (10 by default; 0 doesn't follow), and `client_request::max_redirects`
(`std::optional<std::size_t>`) to override it for one request (Open questions 2 and 3).

## Testing

Loopback servers (as `test/http/client.cpp`'s), scripted per path: each status and method (the method and body the
next hop sees, and the body's fields dropped); relative and absolute `Location`s, `..`, a query and fragment kept as
written (a signed-URL-like query with `+`, `%2F` and unsorted parameters arriving byte for byte); the limit (exactly 10
followed; 11 fails; 0 returns the 3xx; the per-request override); 307/308 with `set_body()` (sent again) and with a
streamed body (the 307 returned); credentials across hosts (two servers on 127.0.0.1 and `localhost`, or two ports:
which headers each sees); a 3xx without `Location`; a bad `Location`; an `ftp:` one; a large previous body (its
connection closed, not drained) and a small one (reused: `stats()`); stop during the second hop; HTTP/2 hops (prior
knowledge). `url::resolve()` on RFC 3986's examples; `url::parse()` keeping encodings (and `fuzz_url`).

## Stages

1. **URLs as given.** `url` keeps the path and query encoded (`query` a string), `origin_form()` re-encodes nothing,
   `url::resolve()`. The URL tests, the client's (which build URLs), the guide's URL section, `fuzz_url`.
2. **Following redirects.** As above, with `client_request::get_body`, `client_options::max_redirects` (and the
   per-request override), `client_errc` (`http/client.hpp`, or its own header), the response's URL, and the guide's
   client section.

## Open questions

1. **How `url` keeps the path and query.** (a) Encoded, as given (`query` a string; `parse_query()` for values), as
   above; (b) Go's: decoded fields plus raw ones (`raw_path` when the encoding isn't the default, `raw_query`); (c)
   leave `url` as it is, and send a `Location` as a raw request target the client doesn't re-encode (only redirects
   fixed; `get()` with a signed URL still broken). **Recommended: (a)**: one form, so nothing can disagree, and it fixes
   `get()` too; it changes `url`'s API (`query` from a map to a string), whose only users are the client and tests.
2. **Follow by default?** (a) Yes, up to 10 (Go, reqwest, browsers); (b) no, only when asked (curl's `-L`, hyper has
   none). **Recommended: (a)**: what callers of an HTTP client expect, and the credential rule (6) makes it safe.
3. **How it's configured.** (a) `client_options::max_redirects` plus a per-request override, as above; (b) only the
   client's option (as Go's per-client `CheckRedirect`); (c) a callback per redirect (Go's `CheckRedirect`: sees the
   next request and the ones before, may stop, or return the 3xx). **Recommended: (a)**: a number covers the uses seen
   (don't follow, follow fewer); a callback can come later if something needs one.
4. **Over the limit.** (a) An error (`too_many_redirects`), the 3xx discarded (Go, curl, reqwest); (b) return the last
   3xx as it is. **Recommended: (a)**, in a new `http::client_errc` (with `bad_redirect`), whose category the guide's
   errors section lists. (A caller wanting the 3xx sets the limit to 0.)
5. **Sending a body again (307/308).** (a) `client_request::get_body`, a `std::function<std::unique_ptr<io::reader>()>`
   (Go's `GetBody`), which `set_body()` sets (the string shared, so a copy isn't made per hop); without it, the 307/308
   is returned; (b) only `set_body()`'s bodies, internally (no public factory); (c) never: a 307/308 with a body is
   always returned. **Recommended: (a)**. (It would also let the client's retries send bodies again; not in this plan.)
6. **Credentials on another host.** Which requests keep `Authorization`, `Proxy-Authorization` and `Cookie`: (a) Go's:
   the same host or a subdomain of it, any scheme or port; (b) the same scheme, host and port, or the same host from
   `http` to `https` (an upgrade, default ports); (c) curl's: the same scheme, host and port only. Either way, once
   dropped they stay dropped. **Recommended: (b)**: never to another host (a subdomain can be someone else's, e.g. a
   user's site on a hosting provider), never from `https` down to `http` (which Go allows), but kept across the common
   `http` to `https` upgrade, which (c) would drop.
7. **`Referer`.** (a) Don't send it (curl's default); (b) Go's (the previous URL, without its user info and fragment,
   not from https to http). **Recommended: (a)**: nothing leaks a URL's path to the next host, and a caller who wants it
   can't easily rely on it anyway.
8. **Saying where the response came from.** (a) `client_response::url`, the URL of the request it answers (Go's
   `resp.Request.URL`, reqwest's `Response::url()`); (b) that and the redirects followed (their URLs and statuses);
   (c) nothing. **Recommended: (a)**: the one thing callers need (e.g. resolving the body's relative links); a history
   can come later.

## Out of scope

- Cookies (a jar): the client has none; `Cookie` headers the caller sets are handled by question 6.
- Sending a URL's user info as `Authorization` (the client ignores it today, as before).
- An overall deadline across hops: the request's stop token (e.g. linked to a `scheduler::call_at`) ends every hop.
- Retrying requests with bodies through `get_body` (the client's retries are unchanged).
