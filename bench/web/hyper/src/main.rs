// The TechEmpower plaintext and JSON tests with hyper (on tokio), for bench/web/run.sh.
use std::convert::Infallible;
use std::net::SocketAddr;

use bytes::Bytes;
use http_body_util::Full;
use hyper::body::Incoming;
use hyper::header::{CONTENT_TYPE, SERVER};
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use serde::Serialize;
use tokio::net::TcpListener;

#[derive(Serialize)]
struct Message {
    message: &'static str,
}

fn respond(content_type: &'static str, body: Bytes) -> Response<Full<Bytes>> {
    Response::builder()
        .header(SERVER, "hyper")
        .header(CONTENT_TYPE, content_type)
        .body(Full::new(body))
        .unwrap()
}

async fn handle(req: Request<Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    Ok(match req.uri().path() {
        "/plaintext" => respond("text/plain", Bytes::from_static(b"Hello, World!")),
        "/json" => {
            // serialized for each request, as the benchmark requires.
            let body = serde_json::to_vec(&Message { message: "Hello, World!" }).unwrap();
            respond("application/json", Bytes::from(body))
        }
        _ => Response::builder()
            .status(StatusCode::NOT_FOUND)
            .body(Full::new(Bytes::new()))
            .unwrap(),
    })
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let port = std::env::var("PORT").ok().and_then(|p| p.parse().ok()).unwrap_or(8080);
    let listener = TcpListener::bind(SocketAddr::from(([0, 0, 0, 0], port))).await?;

    loop {
        let (stream, _) = listener.accept().await?;
        stream.set_nodelay(true)?;

        tokio::spawn(async move {
            // pipeline_flush writes the responses to pipelined requests together, as TechEmpower's hyper does.
            let _ = http1::Builder::new()
                .pipeline_flush(true)
                .serve_connection(TokioIo::new(stream), service_fn(handle))
                .await;
        });
    }
}
