use std::{net::SocketAddr, path::PathBuf, sync::Arc};

use axum::{
    extract::{Query, State},
    http::StatusCode,
    response::IntoResponse,
    routing::get,
    Json, Router,
};
use serde::{Deserialize, Serialize};
use tower_http::{services::ServeDir, trace::TraceLayer};

// These modules are intentionally shared with the desktop Viewer. Keeping the
// decoder in one place prevents the Batocera dashboard from interpreting score
// files or NVRAM differently from the Tauri application.
#[path = "../../../companion/src-tauri/src/nvram.rs"]
#[allow(dead_code)]
mod nvram;
#[path = "../../../companion/src-tauri/src/scores.rs"]
#[allow(dead_code)]
mod scores;

const DEFAULT_LISTEN: &str = "0.0.0.0:8080";
const DEFAULT_TABLES_ROOT: &str = "/userdata/roms/vpinball";
const DEFAULT_MAPS_ROOT: &str = "/userdata/system/scoretracker/plugin/maps";
const DEFAULT_WEB_ROOT: &str = "/userdata/system/scoretracker/web";

#[derive(Clone)]
struct AppState {
    tables_root: String,
    maps_root: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PublicConfig {
    tables_root: String,
    maps_root: String,
    read_only: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Health<'a> {
    status: &'a str,
    version: &'a str,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct SourceQuery {
    score_source: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct NvramQuery {
    rom: String,
    score_source: String,
}

fn argument(name: &str, default: &str) -> String {
    let mut args = std::env::args().skip(1);
    while let Some(value) = args.next() {
        if value == name {
            return args.next().unwrap_or_else(|| default.to_owned());
        }
    }
    default.to_owned()
}

async fn health() -> Json<Health<'static>> {
    Json(Health {
        status: "ok",
        version: env!("CARGO_PKG_VERSION"),
    })
}

async fn config(State(state): State<Arc<AppState>>) -> Json<PublicConfig> {
    Json(PublicConfig {
        tables_root: state.tables_root.clone(),
        maps_root: state.maps_root.clone(),
        read_only: true,
    })
}

async fn scan_scores(State(state): State<Arc<AppState>>) -> impl IntoResponse {
    let tables_root = state.tables_root.clone();
    match tokio::task::spawn_blocking(move || scores::scan(&tables_root)).await {
        Ok(Ok(snapshot)) => Json(snapshot).into_response(),
        Ok(Err(error)) => (StatusCode::INTERNAL_SERVER_ERROR, error.to_string()).into_response(),
        Err(error) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("score scan task failed: {error}"),
        )
            .into_response(),
    }
}

async fn resolve_vpx_hash(
    State(state): State<Arc<AppState>>,
    Query(query): Query<SourceQuery>,
) -> impl IntoResponse {
    let tables_root = state.tables_root.clone();
    match tokio::task::spawn_blocking(move || {
        scores::hash_vpx_for_source(&tables_root, &query.score_source)
    })
    .await
    {
        Ok(Ok(hash)) => Json(hash).into_response(),
        Ok(Err(error)) => (StatusCode::BAD_REQUEST, error).into_response(),
        Err(error) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("VPX hash task failed: {error}"),
        )
            .into_response(),
    }
}

async fn load_nvram(
    State(state): State<Arc<AppState>>,
    Query(query): Query<NvramQuery>,
) -> impl IntoResponse {
    let tables_root = state.tables_root.clone();
    let maps_root = state.maps_root.clone();
    match tokio::task::spawn_blocking(move || {
        nvram::load(&tables_root, &maps_root, &query.rom, &query.score_source)
    })
    .await
    {
        Ok(Ok(document)) => Json(document).into_response(),
        Ok(Err(error)) => (StatusCode::BAD_REQUEST, error).into_response(),
        Err(error) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("NVRAM task failed: {error}"),
        )
            .into_response(),
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "scoretracker_server=info,tower_http=info".into()),
        )
        .init();

    let listen = argument("--listen", DEFAULT_LISTEN);
    let tables_root = argument("--tables-root", DEFAULT_TABLES_ROOT);
    let maps_root = argument("--maps-root", DEFAULT_MAPS_ROOT);
    let web_root = PathBuf::from(argument("--web-root", DEFAULT_WEB_ROOT));
    let address: SocketAddr = listen.parse()?;
    let state = Arc::new(AppState {
        tables_root,
        maps_root,
    });

    let app = Router::new()
        .route("/api/health", get(health))
        .route("/api/config", get(config))
        .route("/api/scores", get(scan_scores))
        .route("/api/vpx-hash", get(resolve_vpx_hash))
        .route("/api/nvram", get(load_nvram))
        .fallback_service(ServeDir::new(web_root).append_index_html_on_directories(true))
        .layer(TraceLayer::new_for_http())
        .with_state(state);

    tracing::info!(%address, "ScoreTracker dashboard is listening");
    let listener = tokio::net::TcpListener::bind(address).await?;
    axum::serve(listener, app)
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await?;
    Ok(())
}
