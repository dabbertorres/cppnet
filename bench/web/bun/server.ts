// The TechEmpower plaintext and JSON tests with Bun.serve, for bench/web/run.sh.
//
// NOTE: one process. Bun spreads connections over several with reusePort, which only balances them on Linux.
const port = Number(process.env.PORT ?? 8080);

Bun.serve({
  port,
  fetch(req) {
    // the path, without parsing the whole URL (as TechEmpower's Bun does): req.url is absolute.
    const url = req.url;
    const path = url.slice(url.indexOf("/", url.indexOf("//") + 2));

    if (path === "/plaintext") {
      return new Response("Hello, World!", { headers: { "Content-Type": "text/plain", Server: "Bun" } });
    }
    if (path === "/json") {
      return Response.json({ message: "Hello, World!" }, { headers: { Server: "Bun" } });
    }
    return new Response(null, { status: 404 });
  },
});
