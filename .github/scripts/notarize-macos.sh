#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <artifact> <submission-name>" >&2
  exit 2
fi

artifact="$1"
submission_name="$2"
submission_json="$RUNNER_TEMP/${submission_name}-notary-submission.json"
submission_log="$RUNNER_TEMP/${submission_name}-notary-log.json"

for variable in APPLE_API_KEY_PATH APPLE_API_KEY_ID APPLE_API_ISSUER_ID; do
  if [ -z "${!variable:-}" ]; then
    echo "::error::Required notarization variable $variable is missing."
    exit 1
  fi
done

if [ ! -f "$artifact" ]; then
  echo "::error::Notarization artifact does not exist: $artifact"
  exit 1
fi

echo "Submitting $artifact to Apple's notary service..."
if ! xcrun notarytool submit "$artifact" \
  --key "$APPLE_API_KEY_PATH" \
  --key-id "$APPLE_API_KEY_ID" \
  --issuer "$APPLE_API_ISSUER_ID" \
  --wait \
  --output-format json \
  > "$submission_json"; then
  echo "::error::Apple's notary service rejected the submission command."
  cat "$submission_json"
  exit 1
fi

submission_id="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["id"])' "$submission_json")"
submission_status="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["status"])' "$submission_json")"

echo "Notarization submission $submission_id finished with status: $submission_status"
if xcrun notarytool log "$submission_id" \
  --key "$APPLE_API_KEY_PATH" \
  --key-id "$APPLE_API_KEY_ID" \
  --issuer "$APPLE_API_ISSUER_ID" \
  "$submission_log"; then
  cat "$submission_log"
else
  echo "::warning::The notarization log could not be downloaded."
fi

if [ "$submission_status" != "Accepted" ]; then
  echo "::error::Apple did not accept $artifact for notarization."
  exit 1
fi
