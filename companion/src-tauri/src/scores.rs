use std::{
    fs::{self, File},
    io::{BufReader, Read},
    path::{Path, PathBuf},
};

use chrono::{SecondsFormat, Utc};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use thiserror::Error;
use walkdir::WalkDir;

#[derive(Debug, Error)]
pub enum ScanError {
    #[error("tables folder does not exist or is not a directory: {0}")]
    InvalidRoot(String),
    #[error("could not resolve tables folder {path}: {source}")]
    Canonicalize {
        path: String,
        source: std::io::Error,
    },
}

#[derive(Debug, Deserialize)]
struct ScoresDocument {
    version: u32,
    games: Vec<SourceGame>,
}

#[derive(Debug, Deserialize)]
struct SourceGame {
    #[serde(default)]
    date: String,
    #[serde(default)]
    rom: String,
    scores: Vec<i64>,
    #[serde(default)]
    game_duration: Option<i64>,
    #[serde(default)]
    game_state: Option<Value>,
    #[serde(default)]
    signature: Option<SourceSignature>,
}

#[derive(Debug, Deserialize)]
struct SourceSignature {
    algorithm: String,
    key_id: String,
    value: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameRecord {
    date: String,
    rom: String,
    scores: Vec<i64>,
    game_duration: Option<i64>,
    game_state: Option<Value>,
    table: String,
    source: String,
    source_index: usize,
    vpx_file_name: Option<String>,
    vpx_file_hash: Option<String>,
    signed: bool,
}

const SIGNATURE_ALGORITHM: &str = "ed25519";
const SIGNATURE_KEY_ID: &str = "scoretracker-release-v1";
const SIGNATURE_PUBLIC_KEY_HEX: &str =
    "73a0a766bcaaeccbbd1692b43d8920ba2b372e29d49d99214118a40fedab799b";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanWarning {
    source: String,
    message: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanSnapshot {
    generated_at: String,
    tables_root: String,
    sources_scanned: usize,
    vpx_files_found: usize,
    games: Vec<GameRecord>,
    warnings: Vec<ScanWarning>,
}

pub fn scan(tables_root: &str) -> Result<ScanSnapshot, ScanError> {
    let requested_root = PathBuf::from(tables_root);
    if !requested_root.is_dir() {
        return Err(ScanError::InvalidRoot(tables_root.to_owned()));
    }

    let root = requested_root
        .canonicalize()
        .map_err(|source| ScanError::Canonicalize {
            path: tables_root.to_owned(),
            source,
        })?;

    let mut sources_scanned = 0;
    let mut vpx_files_found = 0;
    let mut games = Vec::new();
    let mut warnings = Vec::new();

    for entry in WalkDir::new(&root).follow_links(false).into_iter() {
        let entry = match entry {
            Ok(entry) => entry,
            Err(error) => {
                warnings.push(ScanWarning {
                    source: error
                        .path()
                        .map(|path| display_source(path, &root))
                        .unwrap_or_else(|| display_source(&root, &root)),
                    message: error.to_string(),
                });
                continue;
            }
        };

        if !entry.file_type().is_file() {
            continue;
        }

        if entry
            .path()
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("vpx"))
        {
            vpx_files_found += 1;
        }

        if entry.file_name() != "scores.json" {
            continue;
        }

        sources_scanned += 1;
        let path = entry.path();
        match read_source(path, &root) {
            Ok(mut source_games) => games.append(&mut source_games),
            Err(message) => warnings.push(ScanWarning {
                source: display_source(path, &root),
                message,
            }),
        }
    }

    games.sort_by(|left, right| right.date.cmp(&left.date));

    Ok(ScanSnapshot {
        generated_at: Utc::now().to_rfc3339_opts(SecondsFormat::Secs, true),
        tables_root: root.to_string_lossy().into_owned(),
        sources_scanned,
        vpx_files_found,
        games,
        warnings,
    })
}

fn read_source(path: &Path, root: &Path) -> Result<Vec<GameRecord>, String> {
    let file = File::open(path).map_err(|error| error.to_string())?;
    let document: ScoresDocument =
        serde_json::from_reader(BufReader::new(file)).map_err(|error| error.to_string())?;

    if document.version != 1 {
        return Err(format!(
            "unsupported scores schema version {}",
            document.version
        ));
    }

    let table = path
        .parent()
        .and_then(Path::file_name)
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| "Unknown table".to_owned());
    let source = display_source(path, root);
    let vpx_path = find_matching_vpx(path);
    let vpx_file_name = vpx_path
        .as_ref()
        .and_then(|path| path.file_name())
        .map(|name| name.to_string_lossy().into_owned());
    // Hashing a VPX means reading the entire table file, which can be hundreds of
    // megabytes. Keep the initial history scan metadata-only so it returns quickly;
    // an exact hash can be requested lazily when media lookup really needs it.
    let vpx_file_hash = None;

    Ok(document
        .games
        .into_iter()
        .enumerate()
        .filter_map(|(source_index, mut game)| {
            let signed = verify_game_signature(&game);
            game.scores.retain(|score| *score > 0);
            if game.scores.is_empty() {
                return None;
            }
            Some(GameRecord {
                date: game.date,
                rom: game.rom,
                scores: game.scores,
                game_duration: game.game_duration,
                game_state: game.game_state,
                table: table.clone(),
                source: source.clone(),
                source_index,
                vpx_file_name: vpx_file_name.clone(),
                vpx_file_hash: vpx_file_hash.clone(),
                signed,
            })
        })
        .collect())
}

fn signature_payload(game: &SourceGame) -> String {
    let mut payload = String::from("scoretracker.game.v1\n");
    payload.push_str(&format!("date {}\n{}\n", game.date.len(), game.date));
    payload.push_str(&format!("rom {}\n{}\n", game.rom.len(), game.rom));
    payload.push_str(&format!(
        "duration {}\nscores {}\n",
        game.game_duration.unwrap_or_default(),
        game.scores.len()
    ));
    for score in &game.scores {
        payload.push_str(&format!("score {score}\n"));
    }
    payload
}

fn decode_hex<const SIZE: usize>(value: &str) -> Option<[u8; SIZE]> {
    if value.len() != SIZE * 2 {
        return None;
    }
    let mut output = [0_u8; SIZE];
    for (index, byte) in output.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16).ok()?;
    }
    Some(output)
}

fn verify_game_signature(game: &SourceGame) -> bool {
    let Some(signature) = &game.signature else {
        return false;
    };
    if signature.algorithm != SIGNATURE_ALGORITHM || signature.key_id != SIGNATURE_KEY_ID {
        return false;
    }

    let Some(public_key) = decode_hex::<32>(SIGNATURE_PUBLIC_KEY_HEX) else {
        return false;
    };
    let Some(signature_bytes) = decode_hex::<64>(&signature.value) else {
        return false;
    };
    let Ok(verifying_key) = VerifyingKey::from_bytes(&public_key) else {
        return false;
    };
    verifying_key
        .verify(
            signature_payload(game).as_bytes(),
            &Signature::from_bytes(&signature_bytes),
        )
        .is_ok()
}

fn find_matching_vpx(scores_path: &Path) -> Option<PathBuf> {
    let directory = scores_path.parent()?;
    let mut candidates = fs::read_dir(directory)
        .ok()?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| {
            path.is_file()
                && path
                    .extension()
                    .is_some_and(|extension| extension.eq_ignore_ascii_case("vpx"))
        })
        .collect::<Vec<_>>();
    candidates.sort();

    if candidates.len() == 1 {
        return candidates.pop();
    }

    let directory_name = directory.file_name()?.to_string_lossy();
    candidates.into_iter().find(|path| {
        path.file_stem()
            .is_some_and(|stem| stem.to_string_lossy().eq_ignore_ascii_case(&directory_name))
    })
}

fn sha256_file(path: &Path) -> Option<String> {
    let mut file = File::open(path).ok()?;
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer).ok()?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Some(format!("{:x}", hasher.finalize()))
}

pub fn hash_vpx_for_source(
    tables_root: &str,
    score_source: &str,
) -> Result<Option<String>, String> {
    let root = PathBuf::from(tables_root)
        .canonicalize()
        .map_err(|error| format!("could not resolve tables folder: {error}"))?;
    let scores_path = root
        .join(score_source)
        .canonicalize()
        .map_err(|error| format!("could not resolve score source: {error}"))?;

    if !scores_path.starts_with(&root)
        || scores_path
            .file_name()
            .map_or(true, |name| name != "scores.json")
    {
        return Err("score source must be a scores.json file inside the tables folder".to_owned());
    }

    Ok(find_matching_vpx(&scores_path)
        .as_deref()
        .and_then(sha256_file))
}

fn display_source(path: &Path, root: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use std::fs;

    use tempfile::tempdir;

    use super::scan;

    #[test]
    fn scans_valid_v1_history() {
        let temp = tempdir().expect("temporary directory");
        let table = temp.path().join("Attack from Mars");
        fs::create_dir(&table).expect("table directory");
        fs::write(
            table.join("scores.json"),
            r#"{"version":1,"games":[{"date":"2026-07-10T10:00:00Z","rom":"afm_113b","scores":[123456],"game_duration":90}]}"#,
        )
        .expect("scores fixture");
        fs::write(table.join("Attack from Mars.vpx"), b"fixture-vpx").expect("VPX fixture");

        let snapshot = scan(temp.path().to_str().expect("UTF-8 path")).expect("valid scan");
        assert_eq!(snapshot.sources_scanned, 1);
        assert_eq!(snapshot.vpx_files_found, 1);
        assert_eq!(snapshot.games.len(), 1);
        assert_eq!(snapshot.games[0].table, "Attack from Mars");
        assert_eq!(snapshot.games[0].scores, vec![123456]);
        assert_eq!(
            snapshot.games[0].vpx_file_name.as_deref(),
            Some("Attack from Mars.vpx")
        );
        assert_eq!(snapshot.games[0].vpx_file_hash, None);
        assert!(!snapshot.games[0].signed);
        assert!(snapshot.warnings.is_empty());
    }

    #[test]
    fn verifies_signed_history_and_rejects_altered_scores() {
        let temp = tempdir().expect("temporary directory");
        let signature = "478766c78d4c177c97a945ba2f713c1fb273364a945d1a40030f36a1f3bd1a1a4a80569d90de46459f643ee1a65d6080016e589e2fa1ecc58d5474d06ef09106";
        let signed = format!(
            r#"{{"version":1,"games":[{{"date":"2026-07-10T10:00:00Z","rom":"afm_113b","scores":[123456],"game_duration":90,"signature":{{"algorithm":"ed25519","key_id":"scoretracker-release-v1","value":"{signature}"}}}}]}}"#
        );
        fs::write(temp.path().join("scores.json"), &signed).expect("signed fixture");

        let snapshot = scan(temp.path().to_str().expect("UTF-8 path")).expect("valid scan");
        assert!(snapshot.games[0].signed);

        fs::write(
            temp.path().join("scores.json"),
            signed.replace("123456", "123457"),
        )
        .expect("altered fixture");
        let snapshot = scan(temp.path().to_str().expect("UTF-8 path")).expect("valid scan");
        assert!(!snapshot.games[0].signed);
    }

    #[test]
    fn hashes_vpx_only_when_explicitly_requested() {
        let temp = tempdir().expect("temporary directory");
        let table = temp.path().join("Attack from Mars");
        fs::create_dir(&table).expect("table directory");
        fs::write(table.join("scores.json"), r#"{"version":1,"games":[]}"#)
            .expect("scores fixture");
        fs::write(table.join("Attack from Mars.vpx"), b"fixture-vpx").expect("VPX fixture");

        let hash = super::hash_vpx_for_source(
            temp.path().to_str().expect("UTF-8 path"),
            "Attack from Mars/scores.json",
        )
        .expect("hash lookup");

        assert_eq!(
            hash.as_deref(),
            Some("40ed2ce1c1f257f30ffae5a08cb59166d4bcf144a881eab71eeb22914872c1b5")
        );
    }

    #[test]
    fn reports_bad_file_and_keeps_scanning() {
        let temp = tempdir().expect("temporary directory");
        let bad = temp.path().join("Bad Table");
        let good = temp.path().join("Good Table");
        fs::create_dir(&bad).expect("bad table directory");
        fs::create_dir(&good).expect("good table directory");
        fs::write(bad.join("scores.json"), "not json").expect("bad fixture");
        fs::write(good.join("scores.json"), r#"{"version":1,"games":[]}"#).expect("good fixture");

        let snapshot = scan(temp.path().to_str().expect("UTF-8 path")).expect("partial scan");
        assert_eq!(snapshot.sources_scanned, 2);
        assert_eq!(snapshot.warnings.len(), 1);
    }

    #[test]
    fn filters_zero_scores_and_empty_games() {
        let temp = tempdir().expect("temporary directory");
        fs::write(
            temp.path().join("scores.json"),
            r#"{"version":1,"games":[{"date":"2026-07-10T10:00:00Z","rom":"demo","scores":[0,4200,0]},{"date":"2026-07-10T11:00:00Z","rom":"demo","scores":[0,0]}]}"#,
        )
        .expect("scores fixture");

        let snapshot = scan(temp.path().to_str().expect("UTF-8 path")).expect("valid scan");
        assert_eq!(snapshot.games.len(), 1);
        assert_eq!(snapshot.games[0].scores, vec![4200]);
    }

    #[test]
    fn counts_all_vpx_files_recursively() {
        let temp = tempdir().expect("temporary directory");
        let nested = temp.path().join("Nested");
        fs::create_dir(&nested).expect("nested directory");
        fs::write(temp.path().join("First.vpx"), b"one").expect("first VPX fixture");
        fs::write(nested.join("Second.VPX"), b"two").expect("second VPX fixture");
        fs::write(nested.join("ignore.txt"), b"three").expect("non-VPX fixture");

        let snapshot = scan(temp.path().to_str().expect("UTF-8 path")).expect("valid scan");
        assert_eq!(snapshot.vpx_files_found, 2);
    }
}
