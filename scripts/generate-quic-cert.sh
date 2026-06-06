#!/bin/bash
# Generate the self-signed certificate pair the QUIC/WebTransport transport
# loads at startup (quic_cert.pem / quic_key.pem in the HLDS install root —
# the server's working directory).
#
# Browsers pin self-signed WebTransport certs via serverCertificateHashes,
# which requires ECDSA and a validity window of at most 14 days — regenerate
# (and re-pin the hash in the web client) at least every two weeks.
#
# Usage: generate-quic-cert.sh [output-dir] [common-name]

set -e

OUT_DIR="${1:-.}"
CN="${2:-localhost}"

mkdir -p "$OUT_DIR"

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$OUT_DIR/quic_key.pem" -out "$OUT_DIR/quic_cert.pem" -days 14 -nodes \
    -subj "/CN=$CN" \
    -addext "subjectAltName=DNS:$CN,IP:127.0.0.1" 2>/dev/null

CERT_HASH=$(openssl x509 -in "$OUT_DIR/quic_cert.pem" -noout -fingerprint -sha256 | sed 's/.*=//;s/://g' | tr 'A-F' 'a-f')

echo "Wrote $OUT_DIR/quic_cert.pem + $OUT_DIR/quic_key.pem (CN=$CN, 14-day validity)"
echo "SHA-256 hash for serverCertificateHashes: $CERT_HASH"
