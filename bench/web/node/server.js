// The TechEmpower plaintext and JSON tests with Node's http module, for bench/web/run.sh: a worker process per CPU,
// which the primary hands connections to.
const cluster = require("node:cluster");
const http = require("node:http");
const os = require("node:os");

const port = Number(process.env.PORT ?? 8080);

if (cluster.isPrimary) {
  for (let i = 0; i < os.availableParallelism(); i++) cluster.fork();
} else {
  http
    .createServer((req, res) => {
      if (req.url === "/plaintext") {
        res.writeHead(200, { "Content-Type": "text/plain", "Content-Length": 13, Server: "Node" });
        res.end("Hello, World!");
      } else if (req.url === "/json") {
        // serialized for each request, as the benchmark requires.
        const body = JSON.stringify({ message: "Hello, World!" });
        res.writeHead(200, { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body), Server: "Node" });
        res.end(body);
      } else {
        res.writeHead(404);
        res.end();
      }
    })
    .listen(port);
}
