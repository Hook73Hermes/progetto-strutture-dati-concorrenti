#!/bin/bash

# Esegue setup.sh, poi il benchmark con gli argomenti ricevuti, e alla fine (sempre) reset.sh

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0 ..." >&2
    exit 1
fi

cleanup() {
    ./reset.sh
}
trap cleanup EXIT

./setup.sh
./build/pubsub_benchmark "$@"
