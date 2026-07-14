use std::{
    collections::HashMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::SystemTime,
};

use chrono::Utc;
use serde::Serialize;
use serde_json::{json, Map, Value};
use walkdir::WalkDir;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NvramField {
    pub id: String,
    pub section: String,
    pub label: String,
    pub value: Value,
    pub encoding: String,
    pub min: Option<i64>,
    pub max: Option<i64>,
    pub length: usize,
    pub writable: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MachineHighScore {
    pub label: String,
    pub short_label: Option<String>,
    pub initials: String,
    pub score: i64,
    pub initials_field_id: Option<String>,
    pub score_field_id: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NvramDocument {
    pub rom: String,
    pub path: String,
    pub map_path: String,
    pub platform: String,
    pub writable: bool,
    pub write_warning: Option<String>,
    pub checksums_valid: Option<bool>,
    pub high_scores: Vec<MachineHighScore>,
    pub fields: Vec<NvramField>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveResult {
    pub backup_path: String,
    pub checksums_valid: Option<bool>,
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
    label: String,
    encoding: String,
    addresses: Vec<i64>,
    nibble: Option<String>,
    little_endian: bool,
    mask: Option<u8>,
    scale: f64,
    value_offset: f64,
    min: Option<i64>,
    max: Option<i64>,
    char_map: Option<String>,
    null_terminate: bool,
}

#[derive(Debug, Clone)]
struct Checksum {
    coverage_start: i64,
    coverage_end: i64,
    address: i64,
    sixteen_bit: bool,
    big_endian: bool,
}

#[derive(Debug)]
struct MapContext {
    map: Value,
    relative_map_path: String,
    platform: String,
    segments: Vec<Segment>,
    default_little_endian: bool,
    char_map: Option<String>,
    checksums: Vec<Checksum>,
    writable: bool,
    write_warning: Option<String>,
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
    let fields = decode_fields(&context, &registry, &data);
    let high_scores = decode_high_scores(&context, &registry, &data);
    let checksums_valid = validate_checksums(&context, &data);

    Ok(Some(NvramDocument {
        rom: rom.to_owned(),
        path: nvram_path.to_string_lossy().into_owned(),
        map_path: context.relative_map_path,
        platform: context.platform,
        writable: context.writable,
        write_warning: context.write_warning,
        checksums_valid,
        high_scores,
        fields,
    }))
}

pub fn save(
    tables_root: &str,
    maps_root: &str,
    rom: &str,
    nvram_path: &str,
    changes: HashMap<String, Value>,
) -> Result<SaveResult, String> {
    let tables_root = canonical_directory(tables_root, "tables")?;
    let maps_root = canonical_directory(maps_root, "maps")?;
    let path = PathBuf::from(nvram_path)
        .canonicalize()
        .map_err(|error| format!("could not resolve NVRAM path: {error}"))?;
    if !path.starts_with(&tables_root) || !path.is_file() {
        return Err("NVRAM file is outside the configured tables folder".to_owned());
    }

    let context = load_map(&maps_root, rom)?;
    if !context.writable {
        return Err(context
            .write_warning
            .unwrap_or_else(|| "this platform is read-only in the companion".to_owned()));
    }
    let registry = descriptor_registry(&context)?;
    let mut data = fs::read(&path).map_err(|error| format!("could not read NVRAM: {error}"))?;

    for (id, value) in changes {
        let descriptor = registry
            .get(&id)
            .ok_or_else(|| format!("unknown or non-editable NVRAM field: {id}"))?;
        if !descriptor_writable(descriptor) {
            return Err(format!("NVRAM field is read-only: {id}"));
        }
        encode(descriptor, &context, &mut data, &value)?;
    }
    update_checksums(&context, &mut data)?;

    let timestamp = Utc::now().format("%Y%m%dT%H%M%SZ");
    let backup_path = PathBuf::from(format!(
        "{}.scoretracker-backup-{timestamp}",
        path.display()
    ));
    fs::copy(&path, &backup_path).map_err(|error| format!("could not create backup: {error}"))?;

    let mut file = fs::OpenOptions::new()
        .write(true)
        .truncate(true)
        .open(&path)
        .map_err(|error| format!("could not open NVRAM for writing: {error}"))?;
    file.write_all(&data)
        .and_then(|_| file.sync_all())
        .map_err(|error| format!("could not save NVRAM: {error}"))?;

    Ok(SaveResult {
        backup_path: backup_path.to_string_lossy().into_owned(),
        checksums_valid: validate_checksums(&context, &data),
    })
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

    let unsupported_write =
        platform.starts_with("gottlieb-system80") || platform.starts_with("gottlieb-system3");
    let checksums = parse_checksums(&map, !default_little_endian)?;
    Ok(MapContext {
        map,
        relative_map_path: relative.to_owned(),
        platform: platform.clone(),
        segments,
        default_little_endian,
        char_map,
        checksums,
        writable: !unsupported_write,
        write_warning: unsupported_write.then(|| format!("Editing {platform} is disabled until its mirrored-memory rules are fully supported.")),
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
    if let Some(game_state) = context.map.get("game_state").and_then(Value::as_object) {
        for (key, value) in game_state {
            if matches!(
                key.as_str(),
                "scores" | "final_scores" | "current_player" | "current_ball" | "game_over"
            ) {
                continue;
            }
            if value.is_object() {
                registry.insert(format!("game_state.{key}"), descriptor(value, context)?);
            }
        }
    }
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
        label: object
            .get("label")
            .and_then(Value::as_str)
            .unwrap_or("Field")
            .to_owned(),
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
        min: object
            .get("min")
            .map(|value| parse_int(Some(value), 0))
            .transpose()?,
        max: object
            .get("max")
            .map(|value| parse_int(Some(value), 0))
            .transpose()?,
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

fn decode_fields(
    context: &MapContext,
    registry: &HashMap<String, Descriptor>,
    data: &[u8],
) -> Vec<NvramField> {
    let mut ids: Vec<_> = registry
        .keys()
        .filter(|id| id.starts_with("game_state."))
        .cloned()
        .collect();
    ids.sort();
    ids.into_iter()
        .filter_map(|id| {
            let descriptor = registry.get(&id)?;
            let value = decode(descriptor, context, data)?;
            Some(NvramField {
                id,
                section: "game_state".to_owned(),
                label: descriptor.label.clone(),
                value,
                encoding: descriptor.encoding.clone(),
                min: descriptor.min,
                max: descriptor.max,
                length: descriptor.addresses.len(),
                writable: context.writable && descriptor_writable(descriptor),
            })
        })
        .collect()
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
                initials_field_id: registry
                    .get(&initials_id)
                    .is_some_and(descriptor_writable)
                    .then_some(initials_id),
                score_field_id: registry
                    .get(&score_id)
                    .is_some_and(descriptor_writable)
                    .then_some(score_id),
            })
        })
        .collect()
}

fn encode(
    descriptor: &Descriptor,
    context: &MapContext,
    data: &mut [u8],
    value: &Value,
) -> Result<(), String> {
    let resolved = units(descriptor, context)?;
    let count = resolved.len();
    if descriptor.encoding == "ch" {
        let text = value.as_str().unwrap_or_default().to_uppercase();
        let limit = count.saturating_sub(usize::from(descriptor.null_terminate));
        let chars: Vec<char> = text.chars().take(limit).collect();
        for (unit, character) in resolved.iter().zip(chars.iter().copied()) {
            let byte = if let Some(map) = &descriptor.char_map {
                map.chars()
                    .position(|candidate| candidate == character)
                    .unwrap_or(0) as u8
            } else {
                character as u8
            };
            write_unit(data, unit, byte)?;
        }
        if descriptor.null_terminate {
            if let Some(unit) = resolved.get(chars.len()) {
                write_unit(data, unit, 0)?;
            }
        } else {
            for unit in resolved.iter().skip(chars.len()) {
                write_unit(data, unit, b' ')?;
            }
        }
        return Ok(());
    }

    let displayed = value
        .as_bool()
        .map(i64::from)
        .or_else(|| value.as_i64())
        .or_else(|| {
            value
                .as_str()
                .and_then(|text| text.replace(',', "").parse().ok())
        })
        .ok_or("field value is not a number")?;
    if descriptor.min.is_some_and(|min| displayed < min)
        || descriptor.max.is_some_and(|max| displayed > max)
    {
        return Err(format!("{} is outside the mapped range", descriptor.label));
    }
    let number = ((displayed as f64 - descriptor.value_offset) / descriptor.scale)
        .round()
        .max(0.0) as u64;
    let single_nibble = resolved.first().is_some_and(|unit| unit.nibble != "both");
    let mut encoded: Vec<u8> = if descriptor.encoding == "bcd" {
        let digits = if single_nibble { count } else { count * 2 };
        let text = format!("{number:0digits$}");
        if text.len() > digits {
            return Err(format!(
                "{} does not fit in mapped storage",
                descriptor.label
            ));
        }
        if single_nibble {
            text.bytes().map(|byte| byte - b'0').collect()
        } else {
            text.as_bytes()
                .chunks(2)
                .map(|pair| ((pair[0] - b'0') << 4) | (pair[1] - b'0'))
                .collect()
        }
    } else {
        (0..count)
            .rev()
            .map(|shift| ((number >> (shift * 8)) & 0xff) as u8)
            .collect()
    };
    if descriptor.little_endian {
        encoded.reverse();
    }
    for (unit, byte) in resolved.iter().zip(encoded) {
        write_unit(data, unit, byte)?;
    }
    Ok(())
}

fn descriptor_writable(descriptor: &Descriptor) -> bool {
    descriptor.mask.is_none()
        && matches!(descriptor.encoding.as_str(), "int" | "bcd" | "bool" | "ch")
}

fn write_unit(data: &mut [u8], unit: &Unit, value: u8) -> Result<(), String> {
    let byte = data
        .get_mut(unit.offset)
        .ok_or("mapped field is outside the NVRAM file")?;
    *byte = match unit.nibble.as_str() {
        "low" => (*byte & 0xf0) | (value & 0x0f),
        "high" => (*byte & 0x0f) | ((value & 0x0f) << 4),
        _ => value,
    };
    Ok(())
}

fn parse_checksums(map: &Value, big_endian: bool) -> Result<Vec<Checksum>, String> {
    let mut result = Vec::new();
    for (key, sixteen_bit) in [("checksum8", false), ("checksum16", true)] {
        for item in map.get(key).and_then(Value::as_array).into_iter().flatten() {
            let start = parse_int(item.get("start"), 0)?;
            let end = if item.get("end").is_some() {
                parse_int(item.get("end"), start)?
            } else {
                start + parse_int(item.get("length"), 1)? - 1
            };
            let grouping = parse_int(item.get("groupings"), end - start + 1)?.max(1);
            let explicit = item
                .get("checksum")
                .map(|value| parse_int(Some(value), 0))
                .transpose()?;
            let mut group_start = start;
            while group_start <= end {
                let group_end = (group_start + grouping - 1).min(end);
                let bytes = if sixteen_bit { 2 } else { 1 };
                let (coverage_end, address) = explicit
                    .map_or((group_end - bytes, group_end - bytes + 1), |address| {
                        (group_end, address)
                    });
                result.push(Checksum {
                    coverage_start: group_start,
                    coverage_end,
                    address,
                    sixteen_bit,
                    big_endian,
                });
                group_start = group_end + 1;
            }
        }
    }
    Ok(result)
}

fn checksum_value(context: &MapContext, checksum: &Checksum, data: &[u8]) -> Option<u16> {
    let mut value = u16::MAX;
    for address in checksum.coverage_start..=checksum.coverage_end {
        let descriptor = Descriptor {
            label: String::new(),
            encoding: "int".to_owned(),
            addresses: vec![address],
            nibble: None,
            little_endian: false,
            mask: None,
            scale: 1.0,
            value_offset: 0.0,
            min: None,
            max: None,
            char_map: None,
            null_terminate: false,
        };
        let offset = units(&descriptor, context).ok()?.first()?.offset;
        value = value.wrapping_sub(u16::from(*data.get(offset)?));
    }
    Some(if checksum.sixteen_bit {
        value
    } else {
        value & 0xff
    })
}

fn stored_checksum(context: &MapContext, checksum: &Checksum, data: &[u8]) -> Option<u16> {
    let descriptor = Descriptor {
        label: String::new(),
        encoding: "int".to_owned(),
        addresses: vec![checksum.address],
        nibble: None,
        little_endian: false,
        mask: None,
        scale: 1.0,
        value_offset: 0.0,
        min: None,
        max: None,
        char_map: None,
        null_terminate: false,
    };
    let first = units(&descriptor, context).ok()?.first()?.offset;
    if checksum.sixteen_bit {
        let a = u16::from(*data.get(first)?);
        let b = u16::from(*data.get(first + 1)?);
        Some(if checksum.big_endian {
            (a << 8) | b
        } else {
            a | (b << 8)
        })
    } else {
        Some(u16::from(*data.get(first)?))
    }
}

fn validate_checksums(context: &MapContext, data: &[u8]) -> Option<bool> {
    if context.checksums.is_empty() {
        return None;
    }
    Some(context.checksums.iter().all(|checksum| {
        checksum_value(context, checksum, data) == stored_checksum(context, checksum, data)
    }))
}

fn update_checksums(context: &MapContext, data: &mut [u8]) -> Result<(), String> {
    for checksum in &context.checksums {
        let value =
            checksum_value(context, checksum, data).ok_or("checksum coverage is outside NVRAM")?;
        let descriptor = Descriptor {
            label: String::new(),
            encoding: "int".to_owned(),
            addresses: vec![checksum.address],
            nibble: None,
            little_endian: false,
            mask: None,
            scale: 1.0,
            value_offset: 0.0,
            min: None,
            max: None,
            char_map: None,
            null_terminate: false,
        };
        let offset = units(&descriptor, context)?
            .first()
            .ok_or("invalid checksum address")?
            .offset;
        if checksum.sixteen_bit {
            let bytes = if checksum.big_endian {
                value.to_be_bytes()
            } else {
                value.to_le_bytes()
            };
            if offset + 1 >= data.len() {
                return Err("checksum is outside NVRAM".to_owned());
            }
            data[offset] = bytes[0];
            data[offset + 1] = bytes[1];
        } else {
            *data.get_mut(offset).ok_or("checksum is outside NVRAM")? = value as u8;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{find_nvram, load, resolve_maps_root, save};
    use serde_json::json;
    use std::{collections::HashMap, fs, path::Path};
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
    fn reads_and_safely_edits_high_scores() {
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
        fs::write(maps.join("maps/demo.json"), r#"{"_metadata":{"platform":"demo-platform"},"game_state":{"free_play":{"label":"Free Play","start":9,"encoding":"bool"}},"high_scores":[{"label":"Grand Champion","initials":{"start":0,"length":6,"encoding":"ch","null":"terminate"},"score":{"start":6,"length":3,"encoding":"bcd"}}]}"#).expect("map");
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
        let result = save(
            tables.to_str().unwrap(),
            maps.to_str().unwrap(),
            "demo",
            &loaded.path,
            HashMap::from([
                ("high_scores.0.score".to_owned(), json!(654321)),
                ("high_scores.0.initials".to_owned(), json!("XY")),
                ("game_state.free_play".to_owned(), json!(true)),
            ]),
        )
        .expect("save");
        assert!(Path::new(&result.backup_path).is_file());
        let reloaded = load(
            tables.to_str().unwrap(),
            maps.to_str().unwrap(),
            "demo",
            "Demo/scores.json",
        )
        .expect("reload")
        .expect("document");
        assert_eq!(reloaded.high_scores[0].score, 654321);
        assert_eq!(reloaded.high_scores[0].initials, "XY");
        assert_eq!(reloaded.fields[0].value, json!(1));
        let saved = fs::read(nvram_dir.join("demo.nv")).expect("saved NVRAM");
        assert_eq!(&saved[..6], &[b'X', b'Y', 0, 0, 0xff, 0xff]);
    }
}
