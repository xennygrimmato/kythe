#!/bin/bash -e
set -o pipefail

# Copyright 2014 The Kythe Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# This script verifies and formats a single Kythe example, which is expected
# to be piped in on standard input from example.sh.
#
# The script assumes its working directory is the schema output directory and
# requires the following environment variables:
#   TMP
#   LANGUAGE
#   LABEL
#   JAVA_INDEXER_BIN
#   VERIFIER_BIN
#   VERIFIER_ARGS
#   SHASUM_TOOL
#   SHOWGRAPH
#
# TODO(zarko): Provide alternate templates to avoid unnecessary boilerplate
# (eg, text within class scope, text within method scope, ...).

# Save the Java source example
TEST_FILE="$TMP/E.java"
tee "$TEST_FILE.orig" > "$TEST_FILE"
FILE_SHA=$($SHASUM_TOOL "${TEST_FILE}.orig" | cut -c 1-64)

if ! env \
  KYTHE_OUTPUT_FILE="${TEST_FILE}.kzip" \
  KYTHE_ROOT_DIRECTORY="$PWD" \
  "$JAVA_EXTRACTOR_BIN" "$TEST_FILE"; then
  error EXTRACT
fi

# Index the file.
if ! "$JAVA_INDEXER_BIN" "${TEST_FILE}.kzip" >"${TEST_FILE}.entries"; then
  error INDEX
fi

# Verify the index.
if ! "$VERIFIER_BIN" "${VERIFIER_ARGS}" --ignore_dups "${TEST_FILE}.orig" <"${TEST_FILE}.entries"; then
  error VERIFY
fi

# Format the output.
trap 'error FORMAT' ERR

echo "<div><h5 id=\"_${LABEL}\">${LABEL}"

if [[ "${SHOWGRAPH}" == 1 ]]; then
  "$VERIFIER_BIN" --ignore_dups --graphviz >"$TMP/${FILE_SHA}.dot" <"${TEST_FILE}.entries"
  dot -Tsvg -o "${FILE_SHA}.svg" "$TMP/${FILE_SHA}.dot"
  echo "(<a href=\"${FILE_SHA}.svg\" target=\"_blank\">${LANGUAGE}</a>)</h5>"
else
  echo " (${LANGUAGE})</h5>"
fi

source-highlight --failsafe --output=STDOUT --src-lang java -i "${TEST_FILE}.orig"
echo "</div>"
