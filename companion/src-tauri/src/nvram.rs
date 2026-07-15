use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
    time::SystemTime,
};

use serde::Serialize;
use serde_json::{json, Map, Value};
use walkdir::WalkDir;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MachineHighScore {
    pub label: String,
    pub short_label: Option<String>,
    pub initials: String,
    pub score: i64,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NvramDocument {
    pub rom: String,
    pub high_scores: Vec<MachineHighScore>,
}

pub fn resolve_maps_root(selected: &str) -> Result<String, String> {
    let selected = PathBuf::from(selected);
    if !selected.is_dir() {
        return Err("selected maps folder does not exist".to_owned());
    }

    let mut candidates = vec![selected.clone()];
    if selected
        .file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| {
            name.eq_ignore_ascii_case("maps") || name.eq_ignore_ascii_case("platforms")
        })
    {
        if let Some(parent) = selected.parent() {
            candidates.push(parent.to_owned());
        }
    }

    for candidate in candidates {
        if candidate.join("index.json").is_file()
            && candidate.join("maps").is_dir()
            && candidate.join("platforms").is_dir()
        {
            return candidate
                .canonicalize()
                .map(|path| path.to_string_lossy().into_owned())
                .map_err(|error| format!("could not resolve maps folder: {error}"));
        }
    }

    Err("choose the maps root containing index.json, maps/, and platforms/".to_owned())
}

#[derive(Debug, Clone)]
struct Segment {
    address: i64,
    size: i64,
    file_base: usize,
    nibble: String,
}

#[derive(Debug, Clone)]
struct Unit {
    offset: usize,
    nibble: String,
}

#[derive(Debug, Clone)]
struct Descriptor {
    encoding: String,
    addresses: Vec<i64>,
    nibble: Option<String>,
    little_endian: bool,
    mask: Option<u8>,
    scale: f64,
    value_offset: f64,
    char_map: Option<String>,
    null_terminate: bool,
}

#[derive(Debug)]
struct MapContext {
    map: Value,
    segments: Vec<Segment>,
    default_little_endian: bool,
    char_map: Option<String>,
}

pub fn load(
    tables_root: &str,
    maps_root: &str,
    rom: &str,
    score_source: &str,
) -> Result<Option<NvramDocument>, String> {
    let tables_root = canonical_directory(tables_root, "tables")?;
    let maps_root = canonical_directory(maps_root, "maps")?;
    let context = load_map(&maps_root, rom)?;
    let Some(nvram_path) = find_nvram(&tables_root, score_source, rom)? else {
        return Ok(None);
    };
    let data = fs::read(&nvram_path)
        .map_err(|error| format!("could not read {}: {error}", nvram_path.display()))?;
    let registry = descriptor_registry(&context)?;
    let high_scores = decode_high_scores(&context, &registry, &data);

    Ok(Some(NvramDocument {
        rom: rom.to_owned(),
        high_scores,
    }))
}

fn canonical_directory(path: &str, kind: &str) -> Result<PathBuf, String> {
    let path = PathBuf::from(path);
    if !path.is_dir() {
        return Err(format!("configured {kind} folder does not exist"));
    }
    path.canonicalize()
        .map_err(|error| format!("could not resolve {kind} folder: {error}"))
}

fn find_nvram(root: &Path, score_source: &str, rom: &str) -> Result<Option<PathBuf>, String> {
    let source = root.join(score_source);
    let search_root = source
        .parent()
        .filter(|path| path.starts_with(root))
        .unwrap_or(root);

    if let Some(path) = newest_matching_nvram(search_root, rom)? {
        return Ok(Some(path));
    }
    if search_root != root {
        return newest_matching_nvram(root, rom);
    }
    Ok(None)
}

fn newest_matching_nvram(search_root: &Path, rom: &str) -> Result<Option<PathBuf>, String> {
    let mut best: Option<(SystemTime, PathBuf)> = None;
    for entry in WalkDir::new(search_root).max_depth(6).follow_links(false) {
        let entry = entry.map_err(|error| error.to_string())?;
        if !entry.file_type().is_file()
            || !entry
                .path()
                .extension()
                .and_then(|ext| ext.to_str())
                .is_some_and(|ext| ext.eq_ignore_ascii_case("nv"))
        {
            continue;
        }
        let stem = entry
            .path()
            .file_stem()
            .and_then(|stem| stem.to_str())
            .unwrap_or_default();
        if !stem
            .split('-')
            .next()
            .unwrap_or_default()
            .eq_ignore_ascii_case(rom)
        {
            continue;
        }
        let modified = entry
            .metadata()
            .ok()
            .and_then(|metadata| metadata.modified().ok())
            .unwrap_or(SystemTime::UNIX_EPOCH);
        if best.as_ref().is_none_or(|(current, _)| modified > *current) {
            best = Some((modified, entry.path().to_owned()));
        }
    }
    Ok(best.map(|(_, path)| path))
}

fn load_map(root: &Path, rom: &str) -> Result<MapContext, String> {
    let index: Value = read_json(&root.join("index.json"))?;
    let relative = index
        .get(rom)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("no NVRAM map is available for {rom}"))?;
    let map: Value = read_json(&root.join(relative))?;
    let metadata = map
        .get("_metadata")
        .and_then(Value::as_object)
        .ok_or("map has no _metadata")?;
    let platform = metadata
        .get("platform")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    let char_map = metadata
        .get("char_map")
        .and_then(Value::as_str)
        .map(str::to_owned);

    let platform_data = if platform.is_empty() {
        Value::Null
    } else {
        read_json(&root.join("platforms").join(format!("{platform}.json")))?
    };
    let default_little_endian =
        platform_data.get("endian").and_then(Value::as_str) == Some("little");
    let mut file_base = 0usize;
    let mut segments = Vec::new();
    if let Some(layout) = platform_data.get("memory_layout").and_then(Value::as_array) {
        for region in layout {
            if region.get("type").and_then(Value::as_str) != Some("nvram") {
                continue;
            }
            let size = parse_int(region.get("size"), 0)?;
            segments.push(Segment {
                address: parse_int(region.get("address"), 0)?,
                size,
                file_base,
                nibble: region
                    .get("nibble")
                    .and_then(Value::as_str)
                    .unwrap_or("both")
                    .to_owned(),
            });
            file_base += usize::try_from(size).map_err(|_| "invalid NVRAM segment size")?;
        }
    }

    Ok(MapContext {
        map,
        segments,
        default_little_endian,
        char_map,
    })
}

fn read_json(path: &Path) -> Result<Value, String> {
    let text = fs::read_to_string(path)
        .map_err(|error| format!("could not read {}: {error}", path.display()))?;
    serde_json::from_str(&text)
        .map_err(|error| format!("invalid JSON in {}: {error}", path.display()))
}

fn descriptor_registry(context: &MapContext) -> Result<HashMap<String, Descriptor>, String> {
    let mut registry = HashMap::new();
    if let Some(entries) = context.map.get("high_scores").and_then(Value::as_array) {
        for (index, entry) in entries.iter().enumerate() {
            for subkey in ["initials", "score"] {
                if let Some(value) = entry.get(subkey).filter(|value| value.is_object()) {
                    registry.insert(
                        format!("high_scores.{index}.{subkey}"),
                        descriptor(value, context)?,
                    );
                }
            }
        }
    }
    Ok(registry)
}

fn descriptor(value: &Value, context: &MapContext) -> Result<Descriptor, String> {
    let object = value
        .as_object()
        .ok_or("NVRAM descriptor is not an object")?;
    Ok(Descriptor {
        encoding: object
            .get("encoding")
            .and_then(Value::as_str)
            .unwrap_or("int")
            .to_owned(),
        addresses: addresses(object)?,
        nibble: object
            .get("nibble")
            .and_then(Value::as_str)
            .map(str::to_owned),
        little_endian: object
            .get("endian")
            .and_then(Value::as_str)
            .map_or(context.default_little_endian, |endian| endian == "little"),
        mask: object
            .get("mask")
            .map(|value| parse_int(Some(value), 0))
            .transpose()?
            .map(|value| value as u8),
        scale: object.get("scale").and_then(Value::as_f64).unwrap_or(1.0),
        value_offset: object.get("offset").and_then(Value::as_f64).unwrap_or(0.0),
        char_map: context.char_map.clone(),
        null_terminate: object.get("null").and_then(Value::as_str) == Some("terminate"),
    })
}

fn addresses(object: &Map<String, Value>) -> Result<Vec<i64>, String> {
    if let Some(offsets) = object.get("offsets").and_then(Value::as_array) {
        return offsets
            .iter()
            .map(|value| parse_int(Some(value), 0))
            .collect();
    }
    let start = parse_int(object.get("start"), 0)?;
    let end = if let Some(length) = object.get("length") {
        start + parse_int(Some(length), 1)? - 1
    } else {
        parse_int(object.get("end"), start)?
    };
    Ok((start..=end).collect())
}

fn parse_int(value: Option<&Value>, default: i64) -> Result<i64, String> {
    let Some(value) = value else {
        return Ok(default);
    };
    if let Some(number) = value.as_i64() {
        return Ok(number);
    }
    if let Some(boolean) = value.as_bool() {
        return Ok(i64::from(boolean));
    }
    if let Some(text) = value.as_str() {
        return if let Some(hex) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
            i64::from_str_radix(hex, 16).map_err(|error| error.to_string())
        } else {
            text.parse::<i64>().map_err(|error| error.to_string())
        };
    }
    Ok(default)
}

fn units(descriptor: &Descriptor, context: &MapContext) -> Result<Vec<Unit>, String> {
    descriptor
        .addresses
        .iter()
        .map(|address| {
            if context.segments.is_empty() {
                return Ok(Unit {
                    offset: usize::try_from(*address).map_err(|_| "negative NVRAM address")?,
                    nibble: descriptor
                        .nibble
                        .clone()
                        .unwrap_or_else(|| "both".to_owned()),
                });
            }
            let segment = context
                .segments
                .iter()
                .find(|segment| {
                    segment.address <= *address && *address < segment.address + segment.size
                })
                .ok_or_else(|| format!("address 0x{address:X} is outside NVRAM"))?;
            Ok(Unit {
                offset: segment.file_base
                    + usize::try_from(*address - segment.address)
                        .map_err(|_| "invalid NVRAM offset")?,
                nibble: descriptor
                    .nibble
                    .clone()
                    .unwrap_or_else(|| segment.nibble.clone()),
            })
        })
        .collect()
}

fn decode(descriptor: &Descriptor, context: &MapContext, data: &[u8]) -> Option<Value> {
    let resolved = units(descriptor, context).ok()?;
    let mut raw: Vec<u8> = resolved
        .iter()
        .map(|unit| {
            let byte = *data.get(unit.offset)?;
            let value = match unit.nibble.as_str() {
                "low" => byte & 0x0f,
                "high" => byte >> 4,
                _ => byte,
            };
            Some(descriptor.mask.map_or(value, |mask| value & mask))
        })
        .collect::<Option<_>>()?;

    if descriptor.encoding == "ch" {
        let bytes = raw
            .into_iter()
            .take_while(|byte| !descriptor.null_terminate || *byte != 0);
        let text: String = bytes
            .filter_map(|byte| {
                if let Some(map) = &descriptor.char_map {
                    map.chars().nth(byte as usize)
                } else {
                    char::from_u32(byte as u32)
                }
            })
            .filter(|character| !character.is_control())
            .collect();
        return Some(Value::String(text.trim().to_owned()));
    }
    if descriptor.little_endian {
        raw.reverse();
    }
    let single_nibble = resolved.first().is_some_and(|unit| unit.nibble != "both");
    let mut number = 0i64;
    if descriptor.encoding == "bcd" {
        for byte in raw {
            if single_nibble {
                number = number * 10 + i64::from((byte <= 9).then_some(byte).unwrap_or(0));
            } else {
                number = number * 100
                    + i64::from(if byte >> 4 <= 9 { byte >> 4 } else { 0 }) * 10
                    + i64::from(if byte & 0xf <= 9 { byte & 0xf } else { 0 });
            }
        }
    } else {
        for byte in raw {
            number = (number << 8) | i64::from(byte);
        }
        if descriptor.encoding == "bool" {
            number = i64::from(number != 0);
        }
    }
    Some(json!(
        (number as f64 * descriptor.scale + descriptor.value_offset).round() as i64
    ))
}

fn decode_high_scores(
    context: &MapContext,
    registry: &HashMap<String, Descriptor>,
    data: &[u8],
) -> Vec<MachineHighScore> {
    context
        .map
        .get("high_scores")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .enumerate()
        .filter_map(|(index, entry)| {
            let initials_id = format!("high_scores.{index}.initials");
            let score_id = format!("high_scores.{index}.score");
            let initials = registry
                .get(&initials_id)
                .and_then(|descriptor| decode(descriptor, context, data))
                .and_then(|value| value.as_str().map(str::to_owned))
                .unwrap_or_default();
            let score = registry
                .get(&score_id)
                .and_then(|descriptor| decode(descriptor, context, data))
                .and_then(|value| value.as_i64())
                .unwrap_or(0);
            if score <= 0 && initials.is_empty() {
                return None;
            }
            Some(MachineHighScore {
                label: entry
                    .get("label")
                    .and_then(Value::as_str)
                    .unwrap_or("High Score")
                    .to_owned(),
                short_label: entry
                    .get("short_label")
                    .and_then(Value::as_str)
                    .map(str::to_owned),
                initials,
                score,
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{find_nvram, load, resolve_maps_root};
    use std::fs;
    use tempfile::tempdir;

    #[test]
    fn resolves_the_global_maps_root_from_its_maps_child() {
        let temp = tempdir().expect("temporary directory");
        let root = temp.path().join("nvram-maps");
        fs::create_dir_all(root.join("maps")).expect("maps directory");
        fs::create_dir_all(root.join("platforms")).expect("platforms directory");
        fs::write(root.join("index.json"), "{}").expect("index");

        assert_eq!(
            resolve_maps_root(root.join("maps").to_str().unwrap()).expect("resolved root"),
            root.canonicalize().unwrap().to_string_lossy()
        );
    }

    #[test]
    fn falls_back_to_another_table_folder_for_grouped_rom_history() {
        let temp = tempdir().expect("temporary directory");
        let demo = temp.path().join("_ScoreTracker Demo/Attack from Mars Demo");
        let real = temp
            .path()
            .join("Attack from Mars (Bally 1995)/pinmame/nvram");
        fs::create_dir_all(&demo).expect("demo directory");
        fs::create_dir_all(&real).expect("real NVRAM directory");
        fs::write(demo.join("scores.json"), "{}").expect("demo scores");
        let expected = real.join("afm_113b.nv");
        fs::write(&expected, [0_u8; 16]).expect("NVRAM fixture");

        let found = find_nvram(
            temp.path(),
            "_ScoreTracker Demo/Attack from Mars Demo/scores.json",
            "afm_113b",
        )
        .expect("NVRAM search");

        assert_eq!(found.as_deref(), Some(expected.as_path()));
    }

    #[test]
    fn reads_high_scores_without_exposing_write_fields() {
        let temp = tempdir().expect("temporary directory");
        let tables = temp.path().join("tables");
        let maps = temp.path().join("maps");
        let table = tables.join("Demo");
        let nvram_dir = table.join("pinmame/nvram");
        fs::create_dir_all(&nvram_dir).expect("NVRAM directory");
        fs::create_dir_all(maps.join("maps")).expect("maps directory");
        fs::create_dir_all(maps.join("platforms")).expect("platforms directory");
        fs::write(table.join("scores.json"), r#"{"version":1,"games":[]}"#).expect("scores");
        fs::write(maps.join("index.json"), r#"{"demo":"maps/demo.json"}"#).expect("index");
        fs::write(
            maps.join("platforms/demo-platform.json"),
            r#"{"endian":"big","memory_layout":[{"address":0,"size":16,"type":"nvram"}]}"#,
        )
        .expect("platform");
        fs::write(maps.join("maps/demo.json"), r#"{"_metadata":{"platform":"demo-platform"},"high_scores":[{"label":"Grand Champion","initials":{"start":0,"length":6,"encoding":"ch","null":"terminate"},"score":{"start":6,"length":3,"encoding":"bcd"}}]}"#).expect("map");
        fs::write(
            nvram_dir.join("demo.nv"),
            [
                b'A', b'B', b'C', 0, 0xff, 0xff, 0x12, 0x34, 0x56, 0, 0, 0, 0, 0, 0, 0,
            ],
        )
        .expect("NVRAM");

        let loaded = load(
            tables.to_str().unwrap(),
            maps.to_str().unwrap(),
            "demo",
            "Demo/scores.json",
        )
        .expect("load")
        .expect("document");
        assert_eq!(loaded.high_scores[0].initials, "ABC");
        assert_eq!(loaded.high_scores[0].score, 123456);
    }
}
