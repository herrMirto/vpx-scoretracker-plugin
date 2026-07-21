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

notarytool() {
  xcrun notarytool "$@" \
    --key "$APPLE_API_KEY_PATH" \
    --key-id "$APPLE_API_KEY_ID" \
    --issuer "$APPLE_API_ISSUER_ID"
}

json_field() {
  python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8")).get(sys.argv[2], ""))' \
    "$1" "$2"
}

# The GitHub-hosted macOS runners intermittently drop the connection to
# Apple's notary service, which surfaces as the misleading "Internet
# connection appears to be offline" (NSURLErrorNotConnectedToInternet, -1009)
# error. `notarytool submit --wait` aborts the whole build on the first such
# blip, so submit once and poll for the result ourselves, tolerating transient
# network failures on both steps.

submission_id=""
for attempt in 1 2 3 4 5; do
  echo "Submitting $artifact to Apple's notary service (attempt $attempt)..."
  if notarytool submit "$artifact" --output-format json > "$submission_json"; then
    submission_id="$(json_field "$submission_json" id)"
    [ -n "$submission_id" ] && break
  fi
  echo "::warning::Notary submission attempt $attempt failed; retrying in $((attempt * 15))s."
  cat "$submission_json" 2>/dev/null || true
  sleep "$((attempt * 15))"
done

if [ -z "$submission_id" ]; then
  echo "::error::Apple's notary service rejected the submission command."
  exit 1
fi

echo "Notary submission $submission_id created; waiting for the result..."

submission_status="in progress"
for attempt in $(seq 1 90); do
  if notarytool info "$submission_id" --output-format json > "$submission_json"; then
    submission_status="$(json_field "$submission_json" status)"
    echo "Poll $attempt: submission $submission_id status is \"$submission_status\"."
    case "$submission_status" in
      Accepted | Invalid | Rejected)
        break
        ;;
    esac
  else
    echo "::warning::Could not read notary status (poll $attempt); retrying."
  fi
  sleep 30
done

echo "Notarization submission $submission_id finished with status: $submission_status"
if notarytool log "$submission_id" "$submission_log"; then
  cat "$submission_log"
else
  echo "::warning::The notarization log could not be downloaded."
fi

if [ "$submission_status" != "Accepted" ]; then
  echo "::error::Apple did not accept $artifact for notarization."
  exit 1
fi
